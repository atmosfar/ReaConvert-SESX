#ifdef _WIN32
  #define _CRT_SECURE_NO_WARNINGS
  #define strcasecmp _stricmp
#endif

#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <map>
#include <filesystem>

#include "reaper_plugin.h"
#include "reaper_plugin_functions.h"
#include "filebrowse.h"

#include "SesxParser.h"
#include "RppWriter.h"
#include "RppParser.h"
#include "SesxWriter.h"
#include "ProjectInfo.h"
#include "ReaConvUtils.h"
#include "ReaConvert_LoadAPI.h"

namespace fs = std::filesystem;

// Global format metadata
static std::string g_fmt_ext;
static std::string g_fmt_desc;
static std::string g_fmt_id;

// Global pointers
HWND g_hwnd = nullptr;

// Export action
int g_export_cmd_id = 0;

void DoExport() {
    ReaProject* proj = (ReaProject*)EnumProjects(-1, nullptr, 0);
    if (!proj) return;

    char proj_dir_buf[1024] = "", proj_name_buf[1024] = "";
    std::string initial_file_str = "";
    
    if (GetProjectName) {
        GetProjectName(proj, proj_name_buf, sizeof(proj_name_buf));
        if (proj_name_buf[0]) {
            initial_file_str = fs::path(proj_name_buf).stem().string() + "." + g_fmt_ext;
        }
    }

    if (GetProjectPath) {
        GetProjectPath(proj_dir_buf, sizeof(proj_dir_buf));
        
        // If project path is empty (unsaved project), try to use a default or leave empty
        if (proj_dir_buf[0]) {
            char record_path_buf[1024] = "";
            if (GetSetProjectInfo_String && GetSetProjectInfo_String(proj, "RECORD_PATH", record_path_buf, false)) {
                std::string p_path(proj_dir_buf);
                std::string r_path(record_path_buf);
                
                // If record path is relative and matches the end of project path, strip it
                if (!r_path.empty() && fs::path(r_path).is_relative()) {
                    if (p_path.length() >= r_path.length() && 
                        p_path.compare(p_path.length() - r_path.length(), r_path.length(), r_path) == 0) {
                        p_path = p_path.substr(0, p_path.length() - r_path.length());
                        // Trim trailing separator if any
                        while (!p_path.empty() && (p_path.back() == '/' || p_path.back() == '\\')) {
                            p_path.pop_back();
                        }
                        strncpy(proj_dir_buf, p_path.c_str(), sizeof(proj_dir_buf));
                    }
                }
            }
        }
    }

    std::string title = "Export Project to " + g_fmt_desc + " (" + g_fmt_id + ")";
    std::string filter = g_fmt_desc + " files (*." + g_fmt_ext + ")";
    filter.append(1, '\0');
    filter += "*." + g_fmt_ext;
    filter.append(1, '\0');
    filter += "All files (*.*)";
    filter.append(1, '\0');
    filter += "*.*";
    filter.append(1, '\0');

    char save_path[4096] = "";
    if (!WDL_ChooseFileForSave(g_hwnd, title.c_str(), 
                               proj_dir_buf[0] ? proj_dir_buf : nullptr, 
                               initial_file_str.c_str(), 
                               filter.c_str(), 
                               g_fmt_ext.c_str(), false, save_path, sizeof(save_path))) {
        return;
    }

    reaconv::ProjectInfo project;
    reaconv::RppParser rpp_parser;
    if (rpp_parser.Extract(proj, project)) {
        reaconv::SesxWriter writer;
        if (!writer.Write(project, save_path)) {
            if (ShowMessageBox) ShowMessageBox(("Failed to write " + g_fmt_id + " file.").c_str(), (g_fmt_id + " Export Error").c_str(), 0);
        }
    } else {
        if (ShowMessageBox) ShowMessageBox("Failed to extract project data.", (g_fmt_id + " Export Error").c_str(), 0);
    }
}

bool OnAction(int commandIndex, int flag) {
    if (g_export_cmd_id != 0 && commandIndex == g_export_cmd_id) {
        DoExport();
        return true;
    }
    return false;
}

// Register plugin 

static project_import_register_t g_import_reg = {
    [](const char* fn) {
        const char *ext = strrchr(fn, '.');
        return (ext && strcasecmp(ext, ("." + g_fmt_ext).c_str()) == 0);
    },
    [](int i, char **descptr) {
        if (i == 0) {
            static std::string desc;
            desc = g_fmt_desc + " files (*." + g_fmt_ext + ")";
            if (descptr) *descptr = (char*)desc.c_str();
            return g_fmt_id.c_str();
        }
        return (const char*)nullptr;
    },
    [](const char* fn, ProjectStateContext* genstate) {
        reaconv::SesxParser parser;
        reaconv::RppWriter writer;
        reaconv::ProjectInfo project;
        if (parser.Parse(fn, project)) {
            writer.Write(project, genstate);
            return 0; // Success
        }
        return -1;
    }
};

extern "C" REAPER_PLUGIN_DLL_EXPORT int REAPER_PLUGIN_ENTRYPOINT(REAPER_PLUGIN_HINSTANCE hInstance, reaper_plugin_info_t *rec)
{
    if (!rec || rec->caller_version != REAPER_PLUGIN_VERSION) return 0;

    // Initialize format metadata from the parser
    {
        reaconv::SesxParser parser;
        g_fmt_ext = parser.GetFormatFileExtension();
        g_fmt_desc = parser.GetFormatDescription();
        g_fmt_id = g_fmt_ext;
        for (auto & c : g_fmt_id) c = (char)toupper(c);
    }

    g_hwnd = rec->hwnd_main;

    // Load all API functions via the library's loader
    int errcnt = ReaConvert_LoadAPI(rec->GetFunc);

    if (errcnt>0) {
      char buf[256]; snprintf(buf, sizeof(buf), 
          "ReaConvert-%s loaded with %d missing REAPER API functions.\n", g_fmt_id.c_str(), errcnt);
      if (ShowConsoleMsg) ShowConsoleMsg(buf);
    }

    rec->Register("projectimport", &g_import_reg);

    static std::string export_cmd_id_str;
    export_cmd_id_str = g_fmt_id + "_EXPORT_ACTION";
    g_export_cmd_id = rec->Register("command_id", (void*)export_cmd_id_str.c_str());
    
    gaccel_register_t* ac_reg = new gaccel_register_t();
    ac_reg->accel.cmd = g_export_cmd_id;
    static std::string accel_desc;
    accel_desc = g_fmt_id + " Export: Export current project";
    ac_reg->desc = accel_desc.c_str();
    rec->Register("gaccel", ac_reg);

    plugin_register("hookcommand", (void*)OnAction);

    return 1;
}
