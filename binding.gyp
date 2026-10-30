{
  "targets": [
    {
      "target_name": "file_api",
      "sources": [
        "src/domains/file/addon.cpp",
        "src/shared/process_utils.cpp"
      ],
      "include_dirs": [
        "<(module_root_dir)/node_modules/node-addon-api"
      ],
      "dependencies": [
        "<(module_root_dir)/node_modules/node-addon-api/node_addon_api.gyp:node_addon_api_except"
      ],
      "libraries": [
        "Advapi32.lib"
      ],
      "cflags_cc!": [
        "-fno-exceptions"
      ],
      "defines": [
        "NAPI_CPP_EXCEPTIONS"
      ],
      "msvs_settings": {
        "VCCLCompilerTool": {
          "ExceptionHandling": 1
        }
      }
    },
    {
      "target_name": "process_api",
      "sources": [
        "src/domains/process/addon.cpp",
        "src/shared/process_utils.cpp"
      ],
      "include_dirs": [
        "<(module_root_dir)/node_modules/node-addon-api"
      ],
      "dependencies": [
        "<(module_root_dir)/node_modules/node-addon-api/node_addon_api.gyp:node_addon_api_except"
      ],
      "cflags_cc!": [
        "-fno-exceptions"
      ],
      "defines": [
        "NAPI_CPP_EXCEPTIONS"
      ],
      "msvs_settings": {
        "VCCLCompilerTool": {
          "ExceptionHandling": 1
        }
      }
    },
    {
      "target_name": "system_api",
      "sources": [
        "src/domains/system/addon.cpp"
      ],
      "include_dirs": [
        "<(module_root_dir)/node_modules/node-addon-api"
      ],
      "dependencies": [
        "<(module_root_dir)/node_modules/node-addon-api/node_addon_api.gyp:node_addon_api_except"
      ],
      "libraries": [
        "Advapi32.lib",
        "Wbemuuid.lib"
      ],
      "cflags_cc!": [
        "-fno-exceptions"
      ],
      "defines": [
        "NAPI_CPP_EXCEPTIONS"
      ],
      "msvs_settings": {
        "VCCLCompilerTool": {
          "ExceptionHandling": 1
        }
      }
    }
  ]
}
