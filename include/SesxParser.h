#ifndef SESX_PARSER_H
#define SESX_PARSER_H

#include <string>
#include <map>
#include "ProjectInfo.h"
#include "ProjectParser.h"
#include "pugixml.hpp"

namespace reaconv {

class SesxParser : public ProjectParser {
public:
    SesxParser();
    ~SesxParser();

    bool Parse(const std::string& filePath, ProjectInfo& project) override;

private:
    bool ExtractMetadata(const pugi::xml_node& sessionNode, ProjectInfo& project);
    bool ExtractMarkers(const pugi::xml_node& sessionNode, ProjectInfo& project);
    bool ExtractTracks(const pugi::xml_node& sessionNode, ProjectInfo& project);
    bool ExtractClipGroups(const pugi::xml_node& sessionNode, ProjectInfo& project);
    bool BuildFileMap(const pugi::xml_node& sessionNode);
    
    // Helper to find the main <session> node
    pugi::xml_node FindSessionNode(const pugi::xml_document& doc);

    struct SesxFile {
        std::string absolutePath;
        std::string relativePath;
    };
    std::map<std::string, SesxFile> m_fileMap;
    std::string m_sessionPath;
};

} // namespace reaconv

#endif // SESX_PARSER_H
