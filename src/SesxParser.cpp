#include "SesxParser.h"
#include "ReaConvUtils.h"
#include <iostream>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <sstream>

namespace fs = std::filesystem;

namespace reaconv {

// --- SESX Mapping Helpers ---

static double AuditionToDb(double v) {
    static const double coeffs[] = {
        -2.09233708e+04, 1.21079132e+05, -3.04166739e+05, 4.35852234e+05,
        -3.94242081e+05, 2.35794282e+05, -9.52128435e+04, 2.61473496e+04,
        -4.91584283e+03, 6.76296702e+02, -7.34158940e+01
    };
    double dB = 0.0;
    for (int i = 0; i <= 10; ++i) {
        dB += coeffs[i] * std::pow(v, 10 - i);
    }
    return dB;
}

static double AuditionKfToReaperVolEnv(double v) {
    if (v <= 0.000001) return 0.0;
    double dB = AuditionToDb(v);
    if (dB > 6.0) dB = 6.0;
    if (dB < -150.0) return 0.0; 
    return std::pow(10.0, dB / 20.0);
}

static double AuditionKfToReaperPanEnv(double v) {
    // Audition Kf: 0 to 1 -> Reaper: -1 to 1
    return (v * 2.0) - 1.0;
}

static int ConvertAutomationModeToReaper(int mode) {
    switch (mode) {
        case 0: return 0; // Off -> Trim/Read
        case 1: return 1; // Read -> Read
        case 2: return 3; // Write -> Write
        case 3: return 4; // Latch -> Latch
        case 4: return 2; // Touch -> Touch
        default: return 0;
    }
}

static int KeyframeTypeToReaperShape(const std::string& type) {
    if (type == "linear") return 0;
    if (type == "hold") return 1;
    if (type == "bezier") return 5;
    return 0;
}

static void ProcessMuteEnvelope(Envelope& env) {
    if (env.keyframes.empty()) return;
    
    std::vector<Keyframe> raw = env.keyframes;
    env.keyframes.clear();
    
    // In Audition/Reaper Mute: 0 = Unmute, 1 = Mute. Threshold is 0.5.
    double last_state = (raw[0].value > 0.5) ? 1.0 : 0.0;
    
    Keyframe startKf = raw[0];
    startKf.value = last_state;
    startKf.type = "1"; // Force Hold
    env.keyframes.push_back(startKf);
    
    for (size_t i = 0; i < raw.size() - 1; ++i) {
        const auto& p1 = raw[i];
        const auto& p2 = raw[i+1];
        
        // If the segment is already 'hold', just push the next state directly without interpolation
        if (p1.type == "1") {
            double next_state = (p2.value > 0.5) ? 1.0 : 0.0;
            if (next_state != last_state) {
                Keyframe toggleKf = p2;
                toggleKf.value = next_state;
                toggleKf.type = "1";
                env.keyframes.push_back(toggleKf);
                last_state = next_state;
            }
            continue;
        }

        bool crosses = (p1.value <= 0.5 && p2.value > 0.5) || (p1.value >= 0.5 && p2.value < 0.5);
        
        if (crosses && std::abs(p2.value - p1.value) > 0.000001) {
            double t1 = (double)p1.positionSamples;
            double t2 = (double)p2.positionSamples;
            double tc = t1 + (0.5 - p1.value) * (t2 - t1) / (p2.value - p1.value);
            double next_state = (p2.value > 0.5) ? 1.0 : 0.0;
            
            if (next_state != last_state) {
                Keyframe crossingKf;
                crossingKf.positionSamples = (long long)std::round(tc);
                crossingKf.value = next_state;
                crossingKf.type = "1"; // Hold
                env.keyframes.push_back(crossingKf);
                last_state = next_state;
            }
        }
        
        // Always check if the end point of the segment forces a state change not caught by crossing
        // (e.g. abrupt jump or landing exactly on a state)
        double end_state = (p2.value > 0.5) ? 1.0 : 0.0;
        if (end_state != last_state) {
            Keyframe toggleKf = p2;
            toggleKf.value = end_state;
            toggleKf.type = "1";
            env.keyframes.push_back(toggleKf);
            last_state = end_state;
        }
    }
}

static int FadeTypeToShape(const std::string& type) {
    // TODO: Revisit the full category of fade curves/shapes (mapping Audition shapes to Reaper curves)
    return 1; 
}

SesxParser::SesxParser()
  : ProjectParser("Adobe Audition Session", "sesx") {}

SesxParser::~SesxParser() {}

bool SesxParser::Parse(const std::string& filePath, ProjectInfo& project) {
    m_sessionPath = filePath;
    pugi::xml_document doc;
    pugi::xml_parse_result result = doc.load_file(filePath.c_str());
    
    if (!result) {
        std::cerr << "Error: Could not parse SESX file: " << result.description() << std::endl;
        return false;
    }

    pugi::xml_node sessionNode = FindSessionNode(doc);
    if (!sessionNode) {
        std::cerr << "Error: Could not find <session> node in SESX." << std::endl;
        return false;
    }

    if (!BuildFileMap(sessionNode)) return false;
    if (!ExtractMetadata(sessionNode, project)) return false;
    if (!ExtractMarkers(sessionNode, project)) return false;
    if (!ExtractTracks(sessionNode, project)) return false;
    if (!ExtractClipGroups(sessionNode, project)) return false;

    return true;
}

pugi::xml_node SesxParser::FindSessionNode(const pugi::xml_document& doc) {
    pugi::xml_node sesx = doc.child("sesx");
    if (sesx) {
        return sesx.child("session");
    }
    return pugi::xml_node();
}

bool SesxParser::BuildFileMap(const pugi::xml_node& sessionNode) {
    m_fileMap.clear();
    pugi::xml_node filesNode = sessionNode.parent().child("files");
    if (!filesNode) return true;

    for (pugi::xml_node fileNode : filesNode.children("file")) {
        std::string id = fileNode.attribute("id").as_string();
        SesxFile file;
        file.absolutePath = fileNode.attribute("absolutePath").as_string();
        file.relativePath = fileNode.attribute("relativePath").as_string();
        if (!id.empty()) {
            m_fileMap[id] = file;
        }
    }
    return true;
}

bool SesxParser::ExtractMetadata(const pugi::xml_node& sessionNode, ProjectInfo& project) {
    project.sampleRate = sessionNode.attribute("sampleRate").as_int(44100);
    
    project.tempo = 120.0;
    project.timeSigNumerator = 4;
    project.timeSigDenominator = 4;

    pugi::xml_node timeNode = sessionNode.child("sessionState").child("timeFormatState");
    if (timeNode) {
        project.tempo = timeNode.attribute("beatsPerMinute").as_double(120.0);
        project.timeSigNumerator = timeNode.attribute("beatsPerBar").as_int(4);
        project.timeSigDenominator = timeNode.attribute("noteLength").as_int(4);

        double baseFps = timeNode.attribute("timeCodeFrameRate").as_double(24.0);
        bool isNtsc = timeNode.attribute("timeCodeNTSC").as_bool(false);
        project.dropFrame = timeNode.attribute("timeCodeDropFrame").as_bool(false);

        if (isNtsc) {
            if (std::abs(baseFps - 30.0) < 0.01) project.frameRate = 29.97;
            else if (std::abs(baseFps - 24.0) < 0.01) project.frameRate = 23.976;
            else if (std::abs(baseFps - 60.0) < 0.01) project.frameRate = 59.94;
            else project.frameRate = baseFps / 1.001;
        } else {
            project.frameRate = baseFps;
        }
    }

    return true;
}

bool SesxParser::ExtractMarkers(const pugi::xml_node& sessionNode, ProjectInfo& project) {
    pugi::xml_node xmpNode = sessionNode.parent().select_node("//xmpMetadata").node();
    if (!xmpNode) return true;

    std::string xmpText = xmpNode.child_value();
    size_t pos = xmpText.find("<?xpacket end=\"w\"?>");
    if (pos != std::string::npos) {
        xmpText = xmpText.substr(0, pos);
    }

    pugi::xml_document xmpDoc;
    pugi::xml_parse_result xmpResult = xmpDoc.load_string(xmpText.c_str());
    if (!xmpResult) return true;

    pugi::xpath_node_set trackNodes = xmpDoc.select_nodes("//*[local-name()='Tracks']/*[local-name()='Bag']/*[local-name()='li']");
    
    int mIdx = 1;
    for (pugi::xpath_node track_xpath : trackNodes) {
        pugi::xml_node trackLi = track_xpath.node();
        std::string trackName = trackLi.child("xmpDM:trackName").text().get();
        
        if (trackName == "CuePoint Markers") {
            pugi::xpath_node_set xmpMarkers = trackLi.select_nodes(".//*[local-name()='markers']/*[local-name()='Seq']/*[local-name()='li']");
            for (pugi::xpath_node marker_xpath : xmpMarkers) {
                pugi::xml_node markerLi = marker_xpath.node();
                Marker m;
                m.name = markerLi.child("xmpDM:name").text().get();
                
                m.positionSamples = markerLi.child("xmpDM:startTime").attribute("rdf:value").as_llong(0);
                if (m.positionSamples == 0) m.positionSamples = markerLi.child("xmpDM:startTime").text().as_llong(0);
                
                m.durationSamples = markerLi.child("xmpDM:duration").attribute("rdf:value").as_llong(0);
                if (m.durationSamples == 0) m.durationSamples = markerLi.child("xmpDM:duration").text().as_llong(0);
                
                m.index = mIdx++;
                project.markers.push_back(m);
            }
        }
    }

    return true;
}

static void ExtractKeyframesFromParam(const pugi::xml_node& paramNode, Envelope& env, double (*val_conv)(double), long long offsetSamples = 0) {
    pugi::xml_node kfContainer = paramNode.child("parameterKeyframes");
    if (!kfContainer) return;

    for (pugi::xml_node kfNode : kfContainer.children("parameterKeyframe")) {
        Keyframe kf;
        kf.positionSamples = kfNode.attribute("sampleOffset").as_llong(0) - offsetSamples;
        double val = kfNode.attribute("value").as_double(0.0);
        kf.value = val_conv ? val_conv(val) : val;
        kf.type = std::to_string(KeyframeTypeToReaperShape(kfNode.attribute("type").as_string("linear")));
        env.keyframes.push_back(kf);
    }
}

static void ExtractKeyframes(const pugi::xml_node& componentParent, const char* componentID, const char* paramName, Envelope& env, double (*val_conv)(double), bool isActive, long long offsetSamples = 0, long long slotIndex = -1, const pugi::xml_node& editParamParent = pugi::xml_node()) {
    pugi::xml_node comp = componentParent.select_node((std::string(".//component[@componentID='") + componentID + "']").c_str()).node();
    if (!comp) return;

    pugi::xml_node param = comp.select_node((std::string(".//parameter[@name='") + paramName + "']").c_str()).node();
    if (!param) return;

    // Extract component and parameter states
    env.armed = comp.attribute("recordArmed").as_bool(false);

    ExtractKeyframesFromParam(param, env, val_conv, offsetSamples);
    env.active = isActive && !env.keyframes.empty();

    // Visibility: Lane must be open AND have a corresponding editParameter entry
    // If editParamParent is empty, use componentParent (for tracks and clips)
    pugi::xml_node epNode = editParamParent ? editParamParent : componentParent;
    pugi::xml_attribute openAttr = epNode.attribute("automationLaneOpenState");
    bool trackLanesOpen = openAttr ? openAttr.as_bool(false) : true;
    
    bool hasEditParam = false;
    for (pugi::xml_node ep : epNode.children("editParameter")) {
        long long sIdx = ep.attribute("slotIndex").as_llong();
        int pIdx = ep.attribute("parameterIndex").as_int(-1);

        if (slotIndex == -1) {
            // Track/Clip Standard Envelopes
            if (env.name == "volume" && sIdx == 4294967280) hasEditParam = true;
            else if (env.name == "pan" && sIdx == 4294967281) hasEditParam = true;
            else if (env.name == "mute" && sIdx == 4294967282) hasEditParam = true;
        } else {
            // Send Envelopes
            if (env.name == "volume" && sIdx == (4294963200 + slotIndex) && pIdx == 1) hasEditParam = true;
            else if (env.name == "mute" && sIdx == (4294963200 + slotIndex) && pIdx == 0) hasEditParam = true;
            else if (env.name == "pan" && sIdx == (4294963456 + slotIndex) && pIdx == 0) hasEditParam = true;
        }
        if (hasEditParam) break;
    }
    env.visible = trackLanesOpen && hasEditParam;
}

static void ExtractFXChain(const pugi::xml_node& trackNode, std::vector<FX>& fxChain, long long offsetSamples = 0) {
    pugi::xml_node rack = trackNode.child("effectsRack");
    if (!rack) return;

    for (pugi::xml_node slot : rack.children("slot")) {
        pugi::xml_node comp = slot.child("component");
        if (comp) {
            FX fx;
            fx.name = comp.attribute("name").as_string();
            std::string pName = comp.attribute("presetName").as_string();
            if (pName == "(Default)") fx.presetName = "Default";
            else if (pName == "(Custom)") fx.presetName = "";
            else fx.presetName = pName;
            
            fx.powered = slot.attribute("powered").as_bool(true);
            std::string rawPresetBase64 = comp.child("presetChunkData").child_value();
            
            // Sanitize: remove newlines and carriage returns
            rawPresetBase64.erase(std::remove(rawPresetBase64.begin(), rawPresetBase64.end(), '\n'), rawPresetBase64.end());
            rawPresetBase64.erase(std::remove(rawPresetBase64.begin(), rawPresetBase64.end(), '\r'), rawPresetBase64.end());

            std::string rawID = comp.attribute("componentID").as_string();
            size_t colonPos = rawID.find(':');
            if (colonPos != std::string::npos) {
                std::string prefix = rawID.substr(0, colonPos);
                std::string idPart = rawID.substr(colonPos + 1);
                
                if (prefix == "2") {
                    fx.type = FXType::VST2;
                    // VST2 format: "ID Name"
                    size_t spacePos = idPart.find(' ');
                    fx.effectID = (spacePos != std::string::npos) ? idPart.substr(0, spacePos) : idPart;
                    fx.presetChunkData = rawPresetBase64;
                } else if (prefix == "3") {
                    fx.type = FXType::VST3;
                    // VST3 format: "HEX8 HEX8 HEX8 HEX8" -> Need to reconstruct "{HEX32}"
                    // Audition strips leading zeros, so we pad each block back to 8 characters.
                    std::istringstream iss(idPart);
                    std::string segment;
                    std::string fullHex;
                    char buf[9];
                    while (iss >> segment) {
                        unsigned long val = 0;
                        try {
                            val = std::stoul(segment, nullptr, 16);
                        } catch (...) {
                            val = 0;
                        }
                        snprintf(buf, sizeof(buf), "%08X", (unsigned int)val);
                        fullHex += buf;
                    }
                    fx.effectID = "{" + fullHex + "}";

                    // Strip 16-byte Audition wrapper for VST3
                    std::vector<unsigned char> decoded = reaconv::Base64Decode(rawPresetBase64);
                    if (decoded.size() > 16) {
                        fx.presetChunkData = reaconv::Base64Encode(decoded.data() + 16, (unsigned int)decoded.size() - 16);
                    } else {
                        fx.presetChunkData = rawPresetBase64;
                    }
                } else if (prefix == "4") {
                    fx.type = FXType::AU;
                    fx.effectID = idPart;
                    fx.presetChunkData = rawPresetBase64;
                } else {
                    fx.type = FXType::Unknown;
                }
            } else {
                fx.type = FXType::Unknown;
            }

            pugi::xml_attribute openAttr = trackNode.attribute("automationLaneOpenState");
            bool lanesOpen = openAttr ? openAttr.as_bool(false) : true;
            long long slotIdx = slot.attribute("slotIndex").as_llong();

            for (pugi::xml_node paramNode : comp.children("parameter")) {
                Envelope pEnv;
                pEnv.name = paramNode.attribute("name").as_string();
                pEnv.index = paramNode.attribute("index").as_int();
                
                pEnv.active = comp.attribute("powered").as_bool(true);
                pEnv.armed = comp.attribute("recordArmed").as_bool(false);
                
                bool hasEditParam = false;
                for (pugi::xml_node ep : trackNode.children("editParameter")) {
                    if (ep.attribute("slotIndex").as_llong() == slotIdx && 
                        ep.attribute("parameterIndex").as_int() == pEnv.index) {
                        hasEditParam = true;
                        break;
                    }
                }
                pEnv.visible = lanesOpen && hasEditParam;

                ExtractKeyframesFromParam(paramNode, pEnv, nullptr, offsetSamples);
                if (pEnv.active) fx.paramEnvelopes.push_back(pEnv);
            }

            if (fx.type != FXType::Unknown) fxChain.push_back(fx);
        }
    }
}

struct TempSend {
    std::string destID;
    bool preFader;
    float volume;
    float pan;
    std::vector<Envelope> envelopes;
};

bool SesxParser::ExtractTracks(const pugi::xml_node& sessionNode, ProjectInfo& project) {
    pugi::xml_node tracksNode = sessionNode.child("tracks");
    if (!tracksNode) return true;

    std::map<std::string, int> idToReaperIndex;
    std::vector<std::vector<TempSend>> allTrackSends;
    std::string masterID = "10000";

    pugi::xml_node masterNode = sessionNode.child("masterTrack");
    if (!masterNode) masterNode = tracksNode.child("masterTrack");

    if (masterNode) {
        masterID = masterNode.attribute("id").as_string("10000");
        project.masterTrack.trackNumber = -1;
        project.masterTrack.visible = masterNode.attribute("visible").as_bool(true);
        project.masterTrack.sumToMono = masterNode.attribute("sumtomono").as_bool(false);
        project.masterTrack.volume = 1.0f;
        
        pugi::xml_node audioParams = masterNode.child("trackAudioParameters");
        if (audioParams) {
            project.masterTrack.solo = audioParams.attribute("solo").as_bool(false);
        }

        pugi::xml_node volComp = masterNode.select_node(".//component[@componentID='Audition.Fader'][@name='volume']").node();
        if (volComp) {
            project.masterTrack.volume = volComp.select_node(".//parameter[@name='volume']").node().attribute("parameterValue").as_float(1.0f);
        }

        project.masterTrack.mute = false;
        pugi::xml_node muteComp = masterNode.select_node(".//component[@componentID='Audition.Mute']").node();
        if (muteComp) {
            pugi::xml_node muteParam = muteComp.select_node(".//parameter[@name='mute']").node();
            if (!muteParam) muteParam = muteComp.find_child_by_attribute("parameter", "index", "1");
            if (muteParam) {
                project.masterTrack.mute = muteParam.attribute("parameterValue").as_int(0) != 0;
            }
        }

        project.masterTrack.automationMode = ConvertAutomationModeToReaper(audioParams.attribute("automationMode").as_int(1));
        project.masterTrack.automationLaneOpenState = masterNode.attribute("automationLaneOpenState").as_bool(false);
        bool masterEnvsActive = (audioParams.attribute("automationMode").as_int(1) != 0);

        Envelope mVolEnv; mVolEnv.name = "volume";
        ExtractKeyframes(masterNode, "Audition.Fader", "volume", mVolEnv, AuditionKfToReaperVolEnv, masterEnvsActive);
        if (!mVolEnv.keyframes.empty()) project.masterTrack.envelopes.push_back(mVolEnv);

        Envelope mPanEnv; mPanEnv.name = "pan";
        ExtractKeyframes(masterNode, "Audition.StereoPanner", "Pan", mPanEnv, AuditionKfToReaperPanEnv, masterEnvsActive);
        if (!mPanEnv.keyframes.empty()) project.masterTrack.envelopes.push_back(mPanEnv);

        Envelope mMuteEnv; mMuteEnv.name = "mute";
        ExtractKeyframes(masterNode, "Audition.Mute", "mute", mMuteEnv, nullptr, masterEnvsActive);
        if (!mMuteEnv.keyframes.empty()) {
            ProcessMuteEnvelope(mMuteEnv);
            project.masterTrack.envelopes.push_back(mMuteEnv);
        }
        ExtractFXChain(masterNode, project.masterTrack.fxChain);
    }

    for (pugi::xml_node trackNode : tracksNode.children()) {
        std::string nodeName = trackNode.name();
        if (nodeName != "audioTrack" && nodeName != "busTrack") continue;

        Track track;
        track.isBus = (nodeName == "busTrack");
        track.showInMixer = true;
        track.showInArrange = trackNode.attribute("visible").as_bool(true);
        std::string auditionID = trackNode.attribute("id").as_string();
        if (!auditionID.empty()) {
            try { track.trackNumber = std::stoi(auditionID) - std::stoi(masterID); }
            catch(...) { track.trackNumber = (int)project.tracks.size() + 1; }
            idToReaperIndex[auditionID] = (int)project.tracks.size();
        }
        
        pugi::xml_node trackParams = trackNode.child("trackParameters");
        track.name = trackParams.child("name").text().get();
        if (track.name.empty()) track.name = (nodeName == "masterTrack" ? "Master" : (track.isBus ? "Bus" : "Track"));

        float hue = trackParams.attribute("trackHue").as_float(-1.0f);
        track.color = HueToRGB(hue);

        pugi::xml_node audioParams = trackNode.child("trackAudioParameters");
        track.solo = audioParams.attribute("solo").as_bool(false);
        track.armed = audioParams.attribute("recordArmed").as_bool(false);
        track.monitoring = audioParams.attribute("monitoring").as_bool(false);
        track.automationLaneOpenState = trackNode.attribute("automationLaneOpenState").as_bool(false);
        track.automationMode = ConvertAutomationModeToReaper(audioParams.attribute("automationMode").as_int(1));
        bool envsActive = (audioParams.attribute("automationMode").as_int(1) != 0);
        
        std::string outID = audioParams.child("trackOutput").attribute("outputID").as_string();
        if (outID == "10000" || outID == masterID) {
            track.parentTrackNumber = project.masterTrack.trackNumber;
            track.mainSend = true;
        } else if (!outID.empty() && outID != "-1") {
            try { track.parentTrackNumber = std::stoi(outID) - std::stoi(masterID); } 
            catch(...) { track.parentTrackNumber = project.masterTrack.trackNumber; }
            track.mainSend = true;
        } else {
            track.parentTrackNumber = project.masterTrack.trackNumber; // Default to Master as potential parent
            track.mainSend = false;
        }

        track.mute = false;
        pugi::xml_node muteComp = trackNode.select_node(".//component[@componentID='Audition.Mute']").node();
        if (muteComp) {
            pugi::xml_node muteParam = muteComp.select_node(".//parameter[@name='mute']").node();
            if (!muteParam) muteParam = muteComp.find_child_by_attribute("parameter", "index", "1");
            if (muteParam) {
                track.mute = muteParam.attribute("parameterValue").as_int(0) != 0;
            }
        }

        track.volume = 1.0f;
        pugi::xml_node volComp = trackNode.select_node(".//component[@componentID='Audition.Fader'][@name='volume']").node();
        if (volComp) {
            track.volume = volComp.select_node(".//parameter[@name='volume']").node().attribute("parameterValue").as_float(1.0f);
        }

        track.pan = 0.0f;
        pugi::xml_node panComp = trackNode.select_node(".//component[@componentID='Audition.StereoPanner']").node();
        if (panComp) {
            float sesxPan = panComp.select_node(".//parameter[@name='Pan']").node().attribute("parameterValue").as_float(0.0f);
            track.pan = sesxPan / 100.0f;
        }

        // Track Envelopes
        Envelope volEnv; volEnv.name = "volume";
        ExtractKeyframes(trackNode, "Audition.Fader", "volume", volEnv, AuditionKfToReaperVolEnv, envsActive);
        if (!volEnv.keyframes.empty()) track.envelopes.push_back(volEnv);

        Envelope panEnv; panEnv.name = "pan";
        ExtractKeyframes(trackNode, "Audition.StereoPanner", "Pan", panEnv, AuditionKfToReaperPanEnv, envsActive);
        if (!panEnv.keyframes.empty()) track.envelopes.push_back(panEnv);

        Envelope muteEnv; muteEnv.name = "mute";
        ExtractKeyframes(trackNode, "Audition.Mute", "mute", muteEnv, nullptr, envsActive);
        if (!muteEnv.keyframes.empty()) {
            ProcessMuteEnvelope(muteEnv);
            track.envelopes.push_back(muteEnv);
        }

        // Track FX
        ExtractFXChain(trackNode, track.fxChain);

        // Auxiliary Sends
        std::vector<TempSend> trackSends;
        pugi::xml_node sendList = trackNode.child("sendList");
        if (sendList) {
            int sIdx = 0;
            for (pugi::xml_node sendSlot : sendList.children("sendSlot")) {
                TempSend ts;
                ts.destID = sendSlot.attribute("sendOutputID").as_string();
                ts.preFader = sendSlot.attribute("preFader").as_bool(false);
                ts.volume = 1.0f;
                ts.pan = 0.0f;

                pugi::xml_node sendComp = sendSlot.select_node(".//component[@componentID='Audition.Send']").node();
                if (sendComp) {
                    ts.volume = sendComp.select_node(".//parameter[@name='Level']").node().attribute("parameterValue").as_float(1.0f);
                    Envelope sVol; sVol.name = "volume";
                    ExtractKeyframes(sendSlot, "Audition.Send", "Level", sVol, AuditionKfToReaperVolEnv, envsActive, 0, sIdx, trackNode);
                    if (!sVol.keyframes.empty()) ts.envelopes.push_back(sVol);
                    Envelope sMute; sMute.name = "mute";
                    ExtractKeyframes(sendSlot, "Audition.Send", "Power On/Off", sMute, nullptr, envsActive, 0, sIdx, trackNode);
                    if (!sMute.keyframes.empty()) {
                        ProcessMuteEnvelope(sMute);
                        ts.envelopes.push_back(sMute);
                    }
                }

                pugi::xml_node sendPanComp = sendSlot.select_node(".//component[@componentID='Audition.StereoPanner']").node();
                if (sendPanComp) {
                    ts.pan = sendPanComp.select_node(".//parameter[@name='Pan']").node().attribute("parameterValue").as_float(0.0f) / 100.0f;
                    Envelope sPan; sPan.name = "pan";
                    ExtractKeyframes(sendSlot, "Audition.StereoPanner", "Pan", sPan, AuditionKfToReaperPanEnv, envsActive, 0, sIdx, trackNode);
                    if (!sPan.keyframes.empty()) ts.envelopes.push_back(sPan);
                }                trackSends.push_back(ts);
                sIdx++;
            }
        }        allTrackSends.push_back(trackSends);

        // Clip Extraction
        if (nodeName == "audioTrack") {
            for (pugi::xml_node clipNode : trackNode.children("audioClip")) {
                MediaClip clip;
                clip.name = clipNode.attribute("name").as_string();
                clip.startPositionSamples = clipNode.attribute("startPoint").as_llong(0);
                clip.endPositionSamples = clipNode.attribute("endPoint").as_llong(0);
                clip.sourceOffsetSamples = clipNode.attribute("sourceInPoint").as_llong(0);
                clip.loop = clipNode.attribute("looped").as_bool(true);
                clip.fileID = clipNode.attribute("fileID").as_string();
 // Store fileID for potential lookup or group identification
                
                std::string fID = clip.fileID;
                if (m_fileMap.count(fID)) {
                    const auto& file = m_fileMap[fID];
                    std::string rawPath = !file.relativePath.empty() ? file.relativePath : file.absolutePath;
                    
                    if (!rawPath.empty() && fs::path(rawPath).is_relative()) {
                        fs::path sessionDir = fs::path(m_sessionPath).parent_path();
                        clip.filePath = fs::absolute(sessionDir / rawPath).string();
                    } else {
                        clip.filePath = rawPath;
                    }
                    
                    if (!clip.filePath.empty()) {
                        clip.sourceFormat = GetSourceFormat(clip.filePath.substr(clip.filePath.find_last_of('.') + 1));
                    }
                }

                pugi::xml_node cVolComp = clipNode.select_node(".//component[@componentID='Audition.Fader'][@name='volume']").node();
                if (cVolComp) {
                    clip.volume = cVolComp.select_node(".//parameter[@name='volume']").node().attribute("parameterValue").as_float(1.0f);
                    clip.gain = cVolComp.select_node(".//parameter[@name='static gain']").node().attribute("parameterValue").as_float(1.0f);
                }

                Envelope cVol; cVol.name = "volume";
                ExtractKeyframes(clipNode, "Audition.Fader", "volume", cVol, AuditionKfToReaperVolEnv, true, clip.sourceOffsetSamples);
                if (!cVol.keyframes.empty()) clip.envelopes.push_back(cVol);

                Envelope cPan; cPan.name = "pan";
                ExtractKeyframes(clipNode, "Audition.StereoPanner", "Pan", cPan, AuditionKfToReaperPanEnv, true, clip.sourceOffsetSamples);
                if (!cPan.keyframes.empty()) clip.envelopes.push_back(cPan);

                // Clip FX Rack
                ExtractFXChain(clipNode, clip.fxChain, clip.sourceOffsetSamples);

                // Fades
                clip.fadeInDuration = 0.0;
                clip.fadeInShape = 0;
                pugi::xml_node fadeInNode = clipNode.child("fadeIn");
                if (fadeInNode) {
                    long long fStart = fadeInNode.attribute("startPoint").as_llong(0);
                    long long fEnd = fadeInNode.attribute("endPoint").as_llong(0);
                    clip.fadeInDuration = (double)(fEnd - fStart) / project.sampleRate;
                    clip.fadeInShape = FadeTypeToShape(fadeInNode.attribute("type").as_string("lin"));
                }

                clip.fadeOutDuration = 0.0;
                clip.fadeOutShape = 0;
                pugi::xml_node fadeOutNode = clipNode.child("fadeOut");
                if (fadeOutNode) {
                    long long fStart = fadeOutNode.attribute("startPoint").as_llong(0);
                    long long fEnd = fadeOutNode.attribute("endPoint").as_llong(0);
                    clip.fadeOutDuration = (double)(fEnd - fStart) / project.sampleRate;
                    clip.fadeOutShape = FadeTypeToShape(fadeOutNode.attribute("type").as_string("lin"));
                }

                track.clips.push_back(clip);
            }
        }

        project.tracks.push_back(track);
    }

    // 3. Build Receives
    for (int i = 0; i < (int)project.tracks.size(); ++i) {
        const auto& sourceTrack = project.tracks[i];
        if (sourceTrack.parentTrackNumber >= 0) {
            std::string destAuditionID = std::to_string(sourceTrack.parentTrackNumber + 10000);
            if (idToReaperIndex.count(destAuditionID)) {
                int destIdx = idToReaperIndex[destAuditionID];
                Receive rx;
                rx.sourceTrackIndex = sourceTrack.trackNumber;
                rx.mode = 0; rx.volume = 1.0f; rx.pan = 0.0f;
                project.tracks[destIdx].receives.push_back(rx);
            }
        }
        if (i < (int)allTrackSends.size()) {
            for (const auto& send : allTrackSends[i]) {
                if (idToReaperIndex.count(send.destID)) {
                    int destIdx = idToReaperIndex[send.destID];
                    Receive rx;
                    rx.sourceTrackIndex = sourceTrack.trackNumber;
                    rx.mode = send.preFader ? 3 : 0;
                    rx.volume = send.volume;
                    rx.pan = send.pan;
                    rx.envelopes = send.envelopes;
                    project.tracks[destIdx].receives.push_back(rx);
                }
            }
        }
    }

    return true;
}

bool SesxParser::ExtractClipGroups(const pugi::xml_node& sessionNode, ProjectInfo& project) {
    pugi::xml_node groupsNode = sessionNode.child("clipGroups");
    if (!groupsNode) return true;

    int nextReaperGroup = 1;
    for (pugi::xml_node groupNode : groupsNode.children("clipGroup")) {
        int auditionGroupID = groupNode.attribute("id").as_int();
        float hue = groupNode.attribute("hue").as_float(-1.0f);
        ColorRGB groupColor = HueToRGB(hue);
        int reaperGroupID = nextReaperGroup++;

        for (pugi::xml_node itemNode : groupNode.children("item")) {
            std::string trackID = itemNode.attribute("trackID").as_string();
            int clipIdx = itemNode.attribute("clipID").as_int(); // Index within the track's audioClip list

            // Find the track in our project
            for (auto& track : project.tracks) {
                if (std::to_string(track.trackNumber + 10000) == trackID) {
                    if (clipIdx >= 0 && clipIdx < (int)track.clips.size()) {
                        track.clips[clipIdx].group = reaperGroupID;
                        if (hue >= 0.0f) {
                            track.clips[clipIdx].color = groupColor;
                        }
                    }
                    break;
                }
            }
        }
    }
    return true;
}

} // namespace reaconv
