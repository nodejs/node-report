{
  "targets": [
    {
      "target_name": "nodereport",
      "sources": [ "src/node_report.cc", "src/module.cc" ],
      "include_dirs": [ '<!(node -e "require(\'nan\')")' ],
      "conditions": [
        ["OS=='linux'", {
          "defines": [ "_GNU_SOURCE" ],
          "cflags": [ "-g", "-O2", "-std=c++11", ],
        }],
        ["OS=='win'", {
          "libraries": [ "dbghelp.lib", "Netapi32.lib" ],
          "dll_files": [ "dbghelp.dll", "Netapi32.dll" ],
        }],
        ["OS=='mac'", {
          "xcode_settings": {
            'CLANG_CXX_LIBRARY': 'libc++',
            'CLANG_CXX_LANGUAGE_STANDARD':'c++11',
          }
        }],
      ],
    },
    {
      "target_name": "install",
      "type":"none",
      "dependencies" : [ "nodereport" ],
      "copies": [
        {
          "destination": "<(module_root_dir)",
          "files": ["<(module_root_dir)/build/Release/nodereport.node"]
        }]
    },
  ],
}

