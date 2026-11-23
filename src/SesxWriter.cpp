#include "SesxWriter.h"
#include "ReaConvUtils.h"
#include <filesystem>
#include <map>
#include <set>
#include <cmath>

namespace fs = std::filesystem;

namespace reaconv {
    
// Audition IDs: Master=10000, Track 1=10001
// REAPER: Master=-1, Track 1=1. 
const int MASTER_ID = 10000; 

// --- Volume Mapping Logic (Polynomial based) ---

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

static double DbToAudition(double target_dB) {
    if (target_dB <= -150.0) return 0.0;
    if (target_dB >= 15.0) return 1.0;
    double low = 0.0, high = 1.0;
    for (int i = 0; i < 30; ++i) {
        double mid = (low + high) / 2.0;
        if (AuditionToDb(mid) < target_dB) low = mid;
        else high = mid;
    }
    return (low + high) / 2.0;
}

// --- Keyframe Specific Mappings ---

static double ReaperVolEnvToAuditionKf(double gain) {
    if (gain <= 0.000001) return 0.0;
    double dB = 20.0 * std::log10(gain);
    return DbToAudition(dB);
}

static double ReaperPanEnvToAuditionKf(double pan) {
    // Reaper: -1 to 1 -> Audition Kf: 0 to 1
    return (pan + 1.0) / 2.0;
}

static std::string ReaperShapeToKeyframeType(const std::string& reaperShape) {
    int shape = 0;
    try { shape = std::stoi(reaperShape); } catch(...) {}
    if (shape == 1) return "hold";
    if (shape == 5) return "bezier";
    return "linear";
}

static int ConvertAutomationModeToAudition(int mode) {
    switch (mode) {
        case 0: return 1; // Trim/Read -> Read
        case 1: return 1; // Read -> Read
        case 2: return 4; // Touch -> Touch
        case 3: return 2; // Write -> Write
        case 4: return 3; // Latch -> Latch
        default: return 1;
    }
}

// Escape special XML characters for use inside XML element content
static std::string XmlEscape(const std::string& s) {
    std::string result;
    result.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '&':  result += "&amp;"; break;
            case '<':  result += "&lt;"; break;
            case '>':  result += "&gt;"; break;
            case '"': result += "&quot;"; break;
            case '\'': result += "&apos;"; break;
            default:   result += c; break;
        }
    }
    return result;
}

SesxWriter::SesxWriter()
  : ProjectWriter("Adobe Audition Session", "sesx") {}

SesxWriter::~SesxWriter() {}

bool SesxWriter::Write(const ProjectInfo& project, const std::string& filePath) {
    pugi::xml_document doc;

    // 1. Add XML Declaration
    pugi::xml_node decl = doc.append_child(pugi::node_declaration);
    decl.append_attribute("version") = "1.0";
    decl.append_attribute("encoding") = "UTF-8";

    // 2. Add DOCTYPE
    pugi::xml_node doctype = doc.append_child(pugi::node_doctype);
    doctype.set_value("sesx");

    // 3. Root Node
    pugi::xml_node root = doc.append_child("sesx");
    root.append_attribute("version") = "1.9";

    pugi::xml_node session = root.append_child("session");
    SerializeMetadata(project, session);
    SerializeFiles(project, session, filePath);
    SerializeTracks(project, session);
    SerializeClipGroups(project, session);
    SerializeMarkers(project, session);

    return doc.save_file(filePath.c_str(), "\t");
}

void SesxWriter::SerializeMetadata(const ProjectInfo& project, pugi::xml_node& session) {
    session.append_attribute("sampleRate") = project.sampleRate;
    session.append_attribute("duration") = project.durationSamples;
    session.append_attribute("audioChannelType") = "stereo";
    session.append_attribute("bitDepth") = "32";
    
    // sessionState for tempo
    pugi::xml_node state = session.append_child("sessionState");
    pugi::xml_node time = state.append_child("timeFormatState");
    time.append_attribute("beatsPerMinute") = project.tempo;
    time.append_attribute("beatsPerBar") = project.timeSigNumerator;
    time.append_attribute("noteLength") = project.timeSigDenominator;
}

void SesxWriter::SerializeFiles(const ProjectInfo& project, pugi::xml_node& session, const std::string& sessionFilePath) {
    pugi::xml_node files = session.parent().append_child("files");
    std::set<std::string> uniquePaths;
    int fileID = 1;

    fs::path sessionDir = fs::path(sessionFilePath).parent_path();

    for (const auto& track : project.tracks) {
        for (const auto& clip : track.clips) {
            std::string absolutePath = clip.filePath;
            std::replace(absolutePath.begin(), absolutePath.end(), '\\', '/');
            
            if (uniquePaths.find(absolutePath) == uniquePaths.end()) {
                pugi::xml_node file = files.append_child("file");
                
                std::string relativePath = "";
                try {
                    relativePath = fs::relative(absolutePath, sessionDir).string();
                    std::replace(relativePath.begin(), relativePath.end(), '\\', '/');
                } catch (...) {
                    relativePath = absolutePath;
                }
                
                file.append_attribute("absolutePath") = absolutePath.c_str();
                file.append_attribute("relativePath") = relativePath.c_str();
                file.append_attribute("id") = std::to_string(fileID++).c_str();
                
                // Add mediaHandler based on extension
                std::string ext = fs::path(absolutePath).extension().string();
                for (auto & c: ext) c = (char)tolower(c);
                
                std::string handler = "ImporterHost";
                if (ext == ".wav" || ext == ".bwf") handler = "AmioWav";
                else if (ext == ".aif" || ext == ".aiff" || ext == ".aifc") handler = "AmioAiff";
                else if (ext == ".mp3") handler = "AmioMp3";
                else if (ext == ".flac") handler = "AmioFlac";
                
                file.append_attribute("mediaHandler") = handler.c_str();

                uniquePaths.insert(absolutePath);
            }
        }
    }
}

void SesxWriter::SerializeMarkers(const ProjectInfo& project, pugi::xml_node& session) {
    if (project.markers.empty()) return;

    pugi::xml_node xmp = session.append_child("xmpMetadata");
    std::string xmpData = "<?xpacket begin=\"\" id=\"W5M0MpCehiHzreSzNTczkc9d\"?>\n"
                          "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\">\n"
                          " <rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">\n"
                          "  <rdf:Description rdf:about=\"\" xmlns:xmpDM=\"http://ns.adobe.com/xmp/1.0/DynamicMedia/\">\n"
                          "   <xmpDM:Tracks>\n"
                          "    <rdf:Bag>\n"
                          "     <rdf:li rdf:parseType=\"Resource\">\n"
                          "      <xmpDM:trackName>CuePoint Markers</xmpDM:trackName>\n"
                          "      <xmpDM:trackType>Cue</xmpDM:trackType>\n"
                          "      <xmpDM:frameRate>f" + std::to_string(project.sampleRate) + "</xmpDM:frameRate>\n"
                          "      <xmpDM:markers>\n"
                          "       <rdf:Seq>\n";

    for (const auto& m : project.markers) {
        xmpData += "        <rdf:li rdf:parseType=\"Resource\">\n";
        xmpData += "         <xmpDM:startTime>" + std::to_string(m.positionSamples) + "</xmpDM:startTime>\n";
        if (m.durationSamples > 0) {
            xmpData += "         <xmpDM:duration>" + std::to_string(m.durationSamples) + "</xmpDM:duration>\n";
        }
        xmpData += "         <xmpDM:name>" + XmlEscape(m.name) + "</xmpDM:name>\n";
        xmpData += "        </rdf:li>\n";
    }

    xmpData += "       </rdf:Seq>\n"
               "      </xmpDM:markers>\n"
               "     </rdf:li>\n"
               "    </rdf:Bag>\n"
               "   </xmpDM:Tracks>\n"
               "  </rdf:Description>\n"
               " </rdf:RDF>\n"
               "</x:xmpmeta>\n"
               "<?xpacket end=\"w\"?>";
    
    xmp.append_child(pugi::node_cdata).set_value(xmpData.c_str());
}

void SesxWriter::SerializeTracks(const ProjectInfo& project, pugi::xml_node& session) {
    pugi::xml_node tracks = session.append_child("tracks");
    int idx = 1;
    for (const auto& track : project.tracks) {
        BuildTrackNode(tracks, track, idx++, project);
    }
    BuildMasterTrackNode(tracks, project.masterTrack, idx++, project.sampleRate);
}

void SesxWriter::BuildTrackNode(pugi::xml_node& tracksNode, const Track& track, int index, const ProjectInfo& project) {
    int sampleRate = project.sampleRate;
    pugi::xml_node node = tracksNode.append_child(track.isBus ? "busTrack" : "audioTrack");

    std::string trackID = std::to_string(track.trackNumber + MASTER_ID);
    node.append_attribute("id") = trackID.c_str();
    node.append_attribute("index") = index;
    node.append_attribute("visible") = track.showInArrange ? "true" : "false";
    
    bool anyEnvVisible = track.automationLaneOpenState;
    for(const auto& env : track.envelopes) if(env.visible) anyEnvVisible = true;
    for(const auto& fx : track.fxChain) for(const auto& env : fx.paramEnvelopes) if(env.visible) anyEnvVisible = true;

    // Check sends for visibility
    for (const auto& otherTrack : project.tracks) {
        for (const auto& rx : otherTrack.receives) {
            if (rx.sourceTrackIndex == track.trackNumber) {
                for (const auto& env : rx.envelopes) {
                    if (env.visible || !env.keyframes.empty()) anyEnvVisible = true;
                }
            }
        }
    }

    node.append_attribute("automationLaneOpenState") = anyEnvVisible ? "true" : "false";

    pugi::xml_node params = node.append_child("trackParameters");
    params.append_attribute("trackHeight") = "100";
    params.append_child("name").text().set(track.name.c_str());
    params.append_attribute("trackHue") = RGBToHue(track.color);

    pugi::xml_node audio = node.append_child("trackAudioParameters");
    audio.append_attribute("solo") = track.solo;
    audio.append_attribute("recordArmed") = track.armed;
    audio.append_attribute("automationMode") = ConvertAutomationModeToAudition(track.automationMode);
    
    pugi::xml_node trackOut = audio.append_child("trackOutput");
    if (!track.mainSend) {
        trackOut.append_attribute("outputID") = "-1";
    } else if (track.parentTrackNumber == -1) {
        trackOut.append_attribute("outputID") = std::to_string(MASTER_ID); // Master
    } else if (track.parentTrackNumber >= 0) {
        trackOut.append_attribute("outputID") = std::to_string(track.parentTrackNumber + MASTER_ID);
    } else {
        trackOut.append_attribute("outputID") = std::to_string(MASTER_ID); // Fallback to Master
    }
    trackOut.append_attribute("type") = "trackID";

    // Volume Component
    pugi::xml_node fader = audio.append_child("component");
    fader.append_attribute("componentID") = "Audition.Fader";
    fader.append_attribute("id") = "trackFader";
    fader.append_attribute("name") = "volume";
    fader.append_attribute("powered") = "true";
    fader.append_attribute("componentGuid") = GetGUIDString().c_str();
    
    pugi::xml_node volP = fader.append_child("parameter");
    volP.append_attribute("index") = "0";
    volP.append_attribute("name") = "volume";
    volP.append_attribute("parameterValue") = track.volume; // Linear Gain
    
    bool volVisible = false;
    for(const auto& env : track.envelopes) {
        if(env.name == "volume") {
            if(env.visible) volVisible = true;
            BuildKeyframes(volP, env, ReaperVolEnvToAuditionKf);
        }
    }
    if (volVisible) fader.append_attribute("visible") = "true";

    fader.append_child("parameter").append_attribute("index") = "1";
    fader.last_child().append_attribute("name") = "static gain";
    fader.last_child().append_attribute("parameterValue") = "1";

    // Mute Component
    pugi::xml_node mute = audio.append_child("component");
    mute.append_attribute("componentID") = "Audition.Mute";
    mute.append_attribute("id") = "trackMute";
    mute.append_attribute("name") = "Mute";
    mute.append_attribute("powered") = "true";
    mute.append_attribute("componentGuid") = GetGUIDString().c_str();
    
    pugi::xml_node muteP = mute.append_child("parameter");
    muteP.append_attribute("index") = "1";
    muteP.append_attribute("name") = "mute";
    muteP.append_attribute("parameterValue") = track.mute ? 1 : 0;
    
    bool muteVisible = false;
    for(const auto& env : track.envelopes) {
        if(env.name == "mute") {
            if(env.visible) muteVisible = true;
            BuildKeyframes(muteP, env, nullptr);
        }
    }
    if (muteVisible) mute.append_attribute("visible") = "true";

    // Pan Component
    pugi::xml_node panner = audio.append_child("component");
    panner.append_attribute("componentID") = "Audition.StereoPanner";
    panner.append_attribute("id") = "trackPan";
    panner.append_attribute("name") = "StereoPanner";
    panner.append_attribute("powered") = "true";
    panner.append_attribute("componentGuid") = GetGUIDString().c_str();
    
    pugi::xml_node panP = panner.append_child("parameter");
    panP.append_attribute("index") = "0";
    panP.append_attribute("name") = "Pan";
    panP.append_attribute("parameterValue") = track.pan * 100.0f;
    
    bool panVisible = false;
    for(const auto& env : track.envelopes) {
        if(env.name == "pan") {
            if(env.visible) panVisible = true;
            BuildKeyframes(panP, env, ReaperPanEnvToAuditionKf);
        }
    }
    if (panVisible) panner.append_attribute("visible") = "true";

    BuildFXRack(node, track.fxChain);

    // Automation Lane Visibility (editParameter nodes)
    bool epAdded = false;
    if (volVisible) {
        pugi::xml_node ep = node.append_child("editParameter");
        ep.append_attribute("parameterIndex") = "0";
        ep.append_attribute("slotIndex") = "4294967280";
        epAdded = true;
    }
    if (panVisible) {
        pugi::xml_node ep = node.append_child("editParameter");
        ep.append_attribute("parameterIndex") = "0";
        ep.append_attribute("slotIndex") = "4294967281";
        epAdded = true;
    }
    if (muteVisible) {
        pugi::xml_node ep = node.append_child("editParameter");
        ep.append_attribute("parameterIndex") = "1";
        ep.append_attribute("slotIndex") = "4294967282";
        epAdded = true;
    }

    // Dynamic editParameters for Track FX
    for (size_t f = 0; f < track.fxChain.size(); ++f) {
        for (const auto& env : track.fxChain[f].paramEnvelopes) {
            if (env.visible) {
                pugi::xml_node epFX = node.append_child("editParameter");
                epFX.append_attribute("parameterIndex") = env.index;
                epFX.append_attribute("slotIndex") = (unsigned int)f;
                epAdded = true;
            }
        }
    }
    
    // Fallback if none are visible: Audition usually has a default Volume editParameter
    if (!epAdded) {
        pugi::xml_node ep = node.append_child("editParameter");
        ep.append_attribute("parameterIndex") = "0";
        ep.append_attribute("slotIndex") = "4294967280";
    }

    // Pass 1: Add editParameter nodes for all sends (MUST be before sendList)
    int currentSlotIdx = 0;
    for (const auto& otherTrack : project.tracks) {
        for (const auto& rx : otherTrack.receives) {
            if (rx.sourceTrackIndex == track.trackNumber) {
                for (const auto& env : rx.envelopes) {
                    if (env.name == "mute" && (env.visible || !env.keyframes.empty())) {
                        pugi::xml_node ep = node.append_child("editParameter");
                        ep.append_attribute("parameterIndex") = "0"; // Power On/Off
                        ep.append_attribute("slotIndex") = std::to_string(4294963200 + currentSlotIdx).c_str();
                    }
                    if (env.name == "volume" && (env.visible || !env.keyframes.empty())) {
                        pugi::xml_node ep = node.append_child("editParameter");
                        ep.append_attribute("parameterIndex") = "1"; // Level
                        ep.append_attribute("slotIndex") = std::to_string(4294963200 + currentSlotIdx).c_str();
                    }
                    if (env.name == "pan" && (env.visible || !env.keyframes.empty())) {
                        pugi::xml_node ep = node.append_child("editParameter");
                        ep.append_attribute("parameterIndex") = "0"; // Pan
                        ep.append_attribute("slotIndex") = std::to_string(4294963456 + currentSlotIdx).c_str();
                    }
                }
                currentSlotIdx++;
            }
        }
    }

    // Pass 2: Add sendList and slots
    pugi::xml_node sendList;
    currentSlotIdx = 0;
    for (const auto& otherTrack : project.tracks) {
        for (const auto& rx : otherTrack.receives) {
            if (rx.sourceTrackIndex == track.trackNumber) {
                if (!sendList) sendList = node.append_child("sendList");
                
                pugi::xml_node slot = sendList.append_child("sendSlot");
                slot.append_attribute("preFader") = (rx.mode == 3) ? "true" : "false";
                slot.append_attribute("sendOutputID") = std::to_string(otherTrack.trackNumber + 10000);
                slot.append_attribute("slotIndex") = currentSlotIdx;

                pugi::xml_node sendComp = slot.append_child("component");
                sendComp.append_attribute("componentID") = "Audition.Send";
                sendComp.append_attribute("id") = "sendComponent";
                sendComp.append_attribute("powered") = "true";
                sendComp.append_attribute("componentGuid") = GetGUIDString().c_str();
                
                // Power On/Off (Index 0)
                pugi::xml_node powerP = sendComp.append_child("parameter");
                powerP.append_attribute("index") = "0";
                powerP.append_attribute("name") = "Power On/Off";
                powerP.append_attribute("parameterValue") = "1";
                for (const auto& env : rx.envelopes) {
                    if (env.name == "mute") {
                        BuildKeyframes(powerP, env, nullptr, 0, "hold");
                    }
                }

                // Level (Index 1)
                pugi::xml_node levelP = sendComp.append_child("parameter");
                levelP.append_attribute("index") = "1";
                levelP.append_attribute("name") = "Level";
                levelP.append_attribute("parameterValue") = rx.volume;
                for(const auto& env : rx.envelopes) {
                    if(env.name == "volume") {
                        BuildKeyframes(levelP, env, ReaperVolEnvToAuditionKf);
                    }
                }

                pugi::xml_node panComp = slot.append_child("component");
                panComp.append_attribute("componentID") = "Audition.StereoPanner";
                panComp.append_attribute("id") = "sendPan";
                panComp.append_attribute("name") = "StereoPanner";
                panComp.append_attribute("powered") = "true";
                panComp.append_attribute("componentGuid") = GetGUIDString().c_str();
                pugi::xml_node panP = panComp.append_child("parameter");
                panP.append_attribute("index") = "0";
                panP.append_attribute("name") = "Pan";
                panP.append_attribute("parameterValue") = rx.pan * 100.0f;
                for(const auto& env : rx.envelopes) {
                    if(env.name == "pan") {
                        BuildKeyframes(panP, env, ReaperPanEnvToAuditionKf);
                    }
                }
                
                currentSlotIdx++;
            }
        }
    }

    for (size_t i = 0; i < track.clips.size(); ++i) {
        int headID = -1;
        int tailID = -1;

        if (i > 0) {
            // Check if previous clip overlaps this one
            if (track.clips[i-1].endPositionSamples > track.clips[i].startPositionSamples + (0.001 * sampleRate)) {
                headID = (int)i - 1;
            }
        }

        if (i < track.clips.size() - 1) {
            // Check if this clip overlaps the next one
            if (track.clips[i].endPositionSamples > track.clips[i+1].startPositionSamples + (0.001 * sampleRate)) {
                tailID = (int)i + 1;
            }
        }

        BuildClipNode(node, track.clips[i], (int)i, sampleRate, headID, tailID);
    }
}

void SesxWriter::BuildMasterTrackNode(pugi::xml_node& tracksNode, const MasterTrack& master, int index, int sampleRate) {
    pugi::xml_node node = tracksNode.append_child("masterTrack");
    
    bool anyEnvVisible = master.automationLaneOpenState;
    for(const auto& env : master.envelopes) if(env.visible) anyEnvVisible = true;
    for(const auto& fx : master.fxChain) for(const auto& env : fx.paramEnvelopes) if(env.visible) anyEnvVisible = true;

    node.append_attribute("automationLaneOpenState") = anyEnvVisible ? "true" : "false";
    node.append_attribute("id") = std::to_string(master.trackNumber + 10001).c_str(); // -1 + 10001 = 10000
    node.append_attribute("index") = index;
    node.append_attribute("select") = "false";
    node.append_attribute("sumtomono") = master.sumToMono ? "true" : "false";
    node.append_attribute("visible") = master.visible ? "true" : "false";

    pugi::xml_node params = node.append_child("trackParameters");
    params.append_attribute("trackHeight") = "134";
    params.append_attribute("trackHue") = "-1";
    params.append_attribute("trackMinimized") = "false";
    params.append_child("name").text().set(master.name.c_str());

    pugi::xml_node audio = node.append_child("trackAudioParameters");
    audio.append_attribute("audioChannelType") = "stereo";
    audio.append_attribute("automationMode") = ConvertAutomationModeToAudition(master.automationMode);
    audio.append_attribute("monitoring") = master.monitoring;
    audio.append_attribute("recordArmed") = master.armed;
    audio.append_attribute("solo") = master.solo;
    audio.append_attribute("soloSafe") = "true";

    audio.append_child("trackOutput").append_attribute("outputID") = "1";
    audio.last_child().append_attribute("type") = "hardwareOutput";
    audio.append_child("trackInput").append_attribute("inputID") = "-1";
    
    // Volume Component
    pugi::xml_node fader = audio.append_child("component");
    fader.append_attribute("componentGuid") = GetGUIDString().c_str();
    fader.append_attribute("componentID") = "Audition.Fader";
    fader.append_attribute("id") = "trackFader";
    fader.append_attribute("name") = "volume";
    fader.append_attribute("powered") = "true";
    
    pugi::xml_node volP = fader.append_child("parameter");
    volP.append_attribute("index") = "0";
    volP.append_attribute("name") = "volume";
    volP.append_attribute("parameterValue") = master.volume; // Linear Gain
    
    bool volVisible = false;
    for(const auto& env : master.envelopes) {
        if(env.name == "volume") {
            if(env.visible) volVisible = true;
            BuildKeyframes(volP, env, ReaperVolEnvToAuditionKf);
        }
    }
    if (volVisible) fader.append_attribute("visible") = "true";

    fader.append_child("parameter").append_attribute("index") = "1";
    fader.last_child().append_attribute("name") = "static gain";
    fader.last_child().append_attribute("parameterValue") = "1";

    // Mute Component
    pugi::xml_node mute = audio.append_child("component");
    mute.append_attribute("componentGuid") = GetGUIDString().c_str();
    mute.append_attribute("componentID") = "Audition.Mute";
    mute.append_attribute("id") = "trackMute";
    mute.append_attribute("name") = "Mute";
    mute.append_attribute("powered") = "true";
    
    pugi::xml_node muteP = mute.append_child("parameter");
    muteP.append_attribute("index") = "1";
    muteP.append_attribute("name") = "mute";
    muteP.append_attribute("parameterValue") = master.mute ? 1 : 0;
    
    bool muteVisible = false;
    for(const auto& env : master.envelopes) {
        if(env.name == "mute") {
            if(env.visible) muteVisible = true;
            BuildKeyframes(muteP, env, nullptr);
        }
    }
    if (muteVisible) mute.append_attribute("visible") = "true";

    // Pan Component
    pugi::xml_node panner = audio.append_child("component");
    panner.append_attribute("componentGuid") = GetGUIDString().c_str();
    panner.append_attribute("componentID") = "Audition.StereoPanner";
    panner.append_attribute("id") = "trackPan";
    panner.append_attribute("name") = "StereoPanner";
    panner.append_attribute("powered") = "true";
    
    pugi::xml_node panP = panner.append_child("parameter");
    panP.append_attribute("index") = "0";
    panP.append_attribute("name") = "Pan";
    panP.append_attribute("parameterValue") = master.pan * 100.0f;
    
    bool panVisible = false;
    for(const auto& env : master.envelopes) {
        if(env.name == "pan") {
            if(env.visible) panVisible = true;
            BuildKeyframes(panP, env, ReaperPanEnvToAuditionKf);
        }
    }
    if (panVisible) panner.append_attribute("visible") = "true";

    BuildFXRack(node, master.fxChain);

    // Automation Lane Visibility (editParameter nodes)
    bool epAdded = false;
    if (volVisible) {
        pugi::xml_node ep = node.append_child("editParameter");
        ep.append_attribute("parameterIndex") = "0";
        ep.append_attribute("slotIndex") = "4294967280";
        epAdded = true;
    }
    if (panVisible) {
        pugi::xml_node ep = node.append_child("editParameter");
        ep.append_attribute("parameterIndex") = "0";
        ep.append_attribute("slotIndex") = "4294967281";
        epAdded = true;
    }
    if (muteVisible) {
        pugi::xml_node ep = node.append_child("editParameter");
        ep.append_attribute("parameterIndex") = "1";
        ep.append_attribute("slotIndex") = "4294967282";
        epAdded = true;
    }

    // Dynamic editParameters for Master FX
    for (size_t f = 0; f < master.fxChain.size(); ++f) {
        for (const auto& env : master.fxChain[f].paramEnvelopes) {
            if (env.visible) {
                pugi::xml_node epFX = node.append_child("editParameter");
                epFX.append_attribute("parameterIndex") = env.index;
                epFX.append_attribute("slotIndex") = (unsigned int)f;
                epAdded = true;
            }
        }
    }
    
    // Fallback if none are visible: Audition usually has a default Volume editParameter
    if (!epAdded) {
        pugi::xml_node ep = node.append_child("editParameter");
        ep.append_attribute("parameterIndex") = "0";
        ep.append_attribute("slotIndex") = "4294967280";
    }
}

static std::string FadeShapeToType(int shape) {
    // TODO: Work on "cosine", "shape" values, and asymmetric/symmetric logic
    return "log";
}

void SesxWriter::BuildClipNode(pugi::xml_node& trackNode, const MediaClip& clip, int index, int sampleRate, int headID, int tailID) {
    pugi::xml_node node = trackNode.append_child("audioClip");
    node.append_attribute("clipAutoCrossfade") = "true";
    node.append_attribute("crossFadeHeadClipID") = headID;
    node.append_attribute("crossFadeTailClipID") = tailID;
    node.append_attribute("endPoint") = clip.endPositionSamples;
    
    std::string fID = "1";
    pugi::xml_node files = trackNode.parent().parent().parent().child("files");
    for (pugi::xml_node f : files.children("file")) {
        std::string normFile = f.attribute("absolutePath").value();
        std::string normClip = clip.filePath;
        std::replace(normClip.begin(), normClip.end(), '\\', '/');
        if (normFile == normClip) {
            fID = f.attribute("id").value();
            break;
        }
    }
    node.append_attribute("fileID") = fID.c_str();
    node.append_attribute("hue") = "-1";
    node.append_attribute("id") = index;
    node.append_attribute("lockedInTime") = "false";
    node.append_attribute("looped") = clip.loop ? "true" : "false";
    node.append_attribute("name") = clip.name.c_str();
    node.append_attribute("offline") = "false";
    node.append_attribute("select") = "false";
    node.append_attribute("sourceInPoint") = clip.sourceOffsetSamples;
    node.append_attribute("sourceOutPoint") = clip.sourceOffsetSamples + (clip.endPositionSamples - clip.startPositionSamples);
    node.append_attribute("startPoint") = clip.startPositionSamples;
    node.append_attribute("zOrder") = index;

    // Fades
    pugi::xml_node fadeIn = node.append_child("fadeIn");
    fadeIn.append_attribute("crossFadeLinkType") = "linkedAsymmetric";
    fadeIn.append_attribute("endPoint") = (long long)(clip.fadeInDuration * sampleRate);
    fadeIn.append_attribute("shape") = "19";
    fadeIn.append_attribute("startPoint") = "0";
    fadeIn.append_attribute("type") = FadeShapeToType(clip.fadeInShape).c_str();

    pugi::xml_node fadeOut = node.append_child("fadeOut");
    fadeOut.append_attribute("crossFadeLinkType") = "linkedAsymmetric";
    fadeOut.append_attribute("endPoint") = (long long)(clip.endPositionSamples - clip.startPositionSamples);
    fadeOut.append_attribute("shape") = "19";
    fadeOut.append_attribute("startPoint") = (long long)((clip.endPositionSamples - clip.startPositionSamples) - (clip.fadeOutDuration * sampleRate));
    fadeOut.append_attribute("type") = FadeShapeToType(clip.fadeOutShape).c_str();

    // Clip FX Rack
    BuildFXRack(node, clip.fxChain, clip.sourceOffsetSamples);

    // Clip Components
    // Volume
    pugi::xml_node gain = node.append_child("component");
    gain.append_attribute("componentGuid") = GetGUIDString().c_str();
    gain.append_attribute("componentID") = "Audition.Fader";
    gain.append_attribute("id") = "clipGain";
    gain.append_attribute("name") = "volume";
    gain.append_attribute("powered") = "true";
    
    pugi::xml_node volP = gain.append_child("parameter");
    volP.append_attribute("index") = "0";
    volP.append_attribute("name") = "volume";
    volP.append_attribute("parameterValue") = clip.volume;
    
    bool volEnvPresent = false;
    for(const auto& env : clip.envelopes) {
        if(env.name == "volume") {
            BuildKeyframes(volP, env, ReaperVolEnvToAuditionKf, clip.sourceOffsetSamples);
            volEnvPresent = true;
        }
    }

    if (volEnvPresent) gain.append_attribute("visible") = "true";
    pugi::xml_node staticGainP = gain.append_child("parameter");
    staticGainP.append_attribute("index") = "1";
    staticGainP.append_attribute("name") = "static gain";
    staticGainP.append_attribute("parameterValue") = clip.gain;

    // Mute
    pugi::xml_node mute = node.append_child("component");
    mute.append_attribute("componentGuid") = GetGUIDString().c_str();
    mute.append_attribute("componentID") = "Audition.Mute";
    mute.append_attribute("id") = "clipMute";
    mute.append_attribute("name") = "Mute";
    mute.append_attribute("powered") = "true";
    
    pugi::xml_node muteP = mute.append_child("parameter");
    muteP.append_attribute("index") = "1";
    muteP.append_attribute("name") = "mute";
    muteP.append_attribute("parameterValue") = clip.mute ? "1" : "0";
    
    bool muteEnvPresent = false;
    for(const auto& env : clip.envelopes) {
        if(env.name == "mute") {
            BuildKeyframes(muteP, env, nullptr, clip.sourceOffsetSamples);
            muteEnvPresent = true;
        }
    }
    if (muteEnvPresent) mute.append_attribute("visible") = "true";

    // Pan
    pugi::xml_node panner = node.append_child("component");
    panner.append_attribute("componentGuid") = GetGUIDString().c_str();
    panner.append_attribute("componentID") = "Audition.StereoPanner";
    panner.append_attribute("id") = "clipPan";
    panner.append_attribute("name") = "StereoPanner";
    panner.append_attribute("powered") = "true";
    
    pugi::xml_node panP = panner.append_child("parameter");
    panP.append_attribute("index") = "0";
    panP.append_attribute("name") = "Pan";
    panP.append_attribute("parameterValue") = "0";
    
    bool panEnvPresent = false;
    for(const auto& env : clip.envelopes) {
        if(env.name == "pan") {
            BuildKeyframes(panP, env, ReaperPanEnvToAuditionKf, clip.sourceOffsetSamples);
            panEnvPresent = true;
        }
    }
    if (panEnvPresent) panner.append_attribute("visible") = "true";

    // editParameter nodes (Visibility and Focus)
    // Audition always has Vol and Pan by default
    pugi::xml_node epVol = node.append_child("editParameter");
    epVol.append_attribute("parameterIndex") = "0";
    epVol.append_attribute("slotIndex") = "4294967280";

    pugi::xml_node epPan = node.append_child("editParameter");
    epPan.append_attribute("parameterIndex") = "0";
    epPan.append_attribute("slotIndex") = "4294967281";

    if (muteEnvPresent) {
        pugi::xml_node epMute = node.append_child("editParameter");
        epMute.append_attribute("parameterIndex") = "1";
        epMute.append_attribute("slotIndex") = "4294967282";
    }

    // Dynamic editParameters for Clip FX
    for (size_t f = 0; f < clip.fxChain.size(); ++f) {
        for (const auto& env : clip.fxChain[f].paramEnvelopes) {
            pugi::xml_node epFX = node.append_child("editParameter");
            epFX.append_attribute("parameterIndex") = env.index;
            epFX.append_attribute("slotIndex") = (unsigned int)f;
        }
    }

    pugi::xml_node chanMap = node.append_child("channelMap");
    chanMap.append_child("channel").append_attribute("index") = "0"; chanMap.last_child().append_attribute("sourceIndex") = "0";
    chanMap.append_child("channel").append_attribute("index") = "1"; chanMap.last_child().append_attribute("sourceIndex") = "1";
}

void SesxWriter::BuildKeyframes(pugi::xml_node& paramNode, const Envelope& env, double (*val_conv)(double), int startOffsetSamples, const std::string& forceType) {
    if (env.keyframes.empty()) return;
    pugi::xml_node container = paramNode.append_child("parameterKeyframes");
    for (const auto& kf : env.keyframes) {
        pugi::xml_node k = container.append_child("parameterKeyframe");
        k.append_attribute("sampleOffset") = kf.positionSamples + startOffsetSamples;
        k.append_attribute("value") = val_conv ? val_conv(kf.value) : kf.value;
        k.append_attribute("type") = forceType.empty() ? ReaperShapeToKeyframeType(kf.type).c_str() : forceType.c_str();
    }
}

void SesxWriter::BuildFXRack(pugi::xml_node& trackNode, const std::vector<FX>& fxChain,int startOffsetSamples) {
    if (fxChain.empty()) return;
    pugi::xml_node rack = trackNode.append_child("effectsRack");
    rack.append_attribute("preFader") = "true";

    // Rack Mix Component
    pugi::xml_node rackMix = rack.append_child("component");
    rackMix.append_attribute("componentGuid") = GetGUIDString().c_str();
    rackMix.append_attribute("componentID") = "Audition.ComponentGroup";
    rackMix.append_attribute("id") = "rackMix";
    rackMix.append_attribute("powered") = "true";
    rackMix.append_child("parameter").append_attribute("index") = "0";
    rackMix.last_child().append_attribute("parameterValue") = "0";
    rackMix.append_child("parameter").append_attribute("index") = "1";
    rackMix.last_child().append_attribute("parameterValue") = "0";

    // Rack Input Component
    pugi::xml_node rackInput = rack.append_child("component");
    rackInput.append_attribute("componentGuid") = GetGUIDString().c_str();
    rackInput.append_attribute("id") = "rackInput";
    rackInput.append_attribute("powered") = "true";
    rackInput.append_child("parameter").append_attribute("index") = "0";
    rackInput.last_child().append_attribute("parameterValue") = "1";
    rackInput.append_child("parameter").append_attribute("index") = "1";
    rackInput.last_child().append_attribute("parameterValue") = "0";

    // Rack Output Component
    pugi::xml_node rackOutput = rack.append_child("component");
    rackOutput.append_attribute("componentGuid") = GetGUIDString().c_str();
    rackOutput.append_attribute("id") = "rackOutput";
    rackOutput.append_attribute("powered") = "true";
    rackOutput.append_child("parameter").append_attribute("index") = "0";
    rackOutput.last_child().append_attribute("parameterValue") = "1";
    rackOutput.append_child("parameter").append_attribute("index") = "1";
    rackOutput.last_child().append_attribute("parameterValue") = "0";

    const char* stereoBusConfig = "{\"in\":[{\"layout\":[100,101],\"name\":\"Input\",\"type\":0}],\"out\":[{\"layout\":[100,101],\"name\":\"Output\",\"type\":0}]}";

    int slotIdx = 0;
    for (const auto& fx : fxChain) {
        pugi::xml_node slot = rack.append_child("slot");
        slot.append_attribute("powered") = fx.powered;
        slot.append_attribute("slotIndex") = slotIdx++;

        pugi::xml_node comp = slot.append_child("component");
        comp.append_attribute("componentGuid") = GetGUIDString().c_str();
        
        std::string prefix;
        std::string formattedID = fx.effectID;

        if (fx.type == FXType::VST3) {
            prefix = "3:";
            // Convert {HEX32} to hex8 hex8 hex8 hex8 (stripping leading 0s in each block)
            std::string hex = fx.effectID;
            if (!hex.empty() && hex.front() == '{') hex = hex.substr(1);
            if (!hex.empty() && hex.back() == '}') hex.pop_back();
            
            std::string splitHex;
            for (size_t k = 0; k < hex.length(); k += 8) {
                if (!splitHex.empty()) splitHex += " ";
                std::string block = hex.substr(k, 8);
                std::transform(block.begin(), block.end(), block.begin(), ::tolower);
                size_t firstNonZero = block.find_first_not_of('0');
                if (firstNonZero != std::string::npos) {
                    splitHex += block.substr(firstNonZero);
                } else {
                    splitHex += "0";
                }
            }
            formattedID = splitHex;
        } else if (fx.type == FXType::VST2) {
            prefix = "2:";
            formattedID = fx.effectID + " " + fx.name;
        } else if (fx.type == FXType::AU) {
            prefix = "4:";
            formattedID = fx.effectID;
        }
        
        std::string finalCompID = prefix + formattedID;
        comp.append_attribute("componentID") = finalCompID.c_str();
        comp.append_attribute("name") = fx.name.c_str();
        comp.append_attribute("presetName") = fx.presetName.empty() ? "(Default)" : fx.presetName.c_str();

        comp.append_child("busConfig").append_child(pugi::node_cdata).set_value(stereoBusConfig);

        if (!fx.presetChunkData.empty()) {
            std::vector<unsigned char> decoded = reaconv::Base64Decode(fx.presetChunkData);
            std::vector<unsigned char> finalPayload;
            
            if (fx.type == FXType::VST3) {
                // Prepend 16-byte Audition wrapper: Size (4 bytes LE) + 12 bytes padding
                unsigned int size = (unsigned int)decoded.size();
                unsigned char wrapper[16] = {0};
                wrapper[0] = size & 0xFF;
                wrapper[1] = (size >> 8) & 0xFF;
                wrapper[2] = (size >> 16) & 0xFF;
                wrapper[3] = (size >> 24) & 0xFF;
                
                finalPayload.insert(finalPayload.end(), wrapper, wrapper + 16);
                finalPayload.insert(finalPayload.end(), decoded.begin(), decoded.end());
            } else {
                finalPayload = decoded;
            }

            std::string encoded = reaconv::Base64Encode(finalPayload.data(), (unsigned int)finalPayload.size());
            comp.append_child("presetChunkData").append_child(pugi::node_cdata).set_value(encoded.c_str());
        }

        for (const auto& env : fx.paramEnvelopes) {
            pugi::xml_node paramNode = comp.append_child("parameter");
            paramNode.append_attribute("index") = env.index;
            paramNode.append_attribute("name") = env.name.c_str();
            // Audition expects normalized 0..1 values in parameterValue for the current state if not automated, 
            // but we'll focus on keyframes first.
            BuildKeyframes(paramNode, env, nullptr, startOffsetSamples); // FX params are already normalized by RppParser
        }
    }
}

void SesxWriter::SerializeClipGroups(const ProjectInfo& project, pugi::xml_node& session) {
    std::map<int, std::vector<std::pair<std::string, int>>> groups;
    std::map<int, ColorRGB> groupColors;

    for (const auto& track : project.tracks) {
        for (int i = 0; i < (int)track.clips.size(); ++i) {
            const auto& clip = track.clips[i];
            if (clip.group > 0) {
                std::string trackID = std::to_string(track.trackNumber + MASTER_ID);
                groups[clip.group].push_back({trackID, i});
                if (clip.color.hasColor && groupColors.find(clip.group) == groupColors.end()) {
                    groupColors[clip.group] = clip.color;
                }
            }
        }
    }

    if (groups.empty()) return;

    pugi::xml_node groupsNode = session.append_child("clipGroups");
    for (const auto& [groupID, items] : groups) {
        pugi::xml_node groupNode = groupsNode.append_child("clipGroup");
        groupNode.append_attribute("id") = groupID;
        
        float hue = -1.0f;
        if (groupColors.count(groupID)) {
            hue = (float)RGBToHue(groupColors[groupID]);
        }
        groupNode.append_attribute("hue") = hue;

        for (const auto& item : items) {
            pugi::xml_node itemNode = groupNode.append_child("item");
            itemNode.append_attribute("trackID") = item.first.c_str();
            itemNode.append_attribute("clipID") = item.second;
        }
    }
}

} // namespace reaconv
