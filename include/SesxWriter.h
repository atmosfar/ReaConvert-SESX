#ifndef SESX_WRITER_H
#define SESX_WRITER_H

#include "ProjectInfo.h"
#include "ProjectWriter.h"
#include "pugixml.hpp"

namespace reaconv {

class SesxWriter : public ProjectWriter {
public:
    SesxWriter();
    ~SesxWriter();

    bool Write(const ProjectInfo& project, const std::string& filePath) override;

private:
    void SerializeMetadata(const ProjectInfo& project, pugi::xml_node& sessionNode);
    void SerializeMarkers(const ProjectInfo& project, pugi::xml_node& sessionNode);
    void SerializeFiles(const ProjectInfo& project, pugi::xml_node& sessionNode, const std::string& sessionFilePath);
    void SerializeTracks(const ProjectInfo& project, pugi::xml_node& sessionNode);
    void SerializeClipGroups(const ProjectInfo& project, pugi::xml_node& sessionNode);
    
    void BuildTrackNode(pugi::xml_node& tracksNode, const Track& track, int index, const ProjectInfo& project);
    void BuildMasterTrackNode(pugi::xml_node& tracksNode, const MasterTrack& master, int index, int sampleRate);
    void BuildClipNode(pugi::xml_node& trackNode, const MediaClip& clip, int index, int sampleRate, int headID = -1, int tailID = -1);
    void BuildKeyframes(pugi::xml_node& paramNode, const Envelope& env, double (*val_conv)(double), int startOffsetSamples = 0, const std::string& forceType = "");
    void BuildFXRack(pugi::xml_node& trackNode, const std::vector<FX>& fxChain, int startOffsetSamples = 0);
};

} // namespace reaconv

#endif // SESX_WRITER_H
