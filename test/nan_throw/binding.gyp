{
  "targets": [
    {
      "target_name": "test_nan_throw",
      "include_dirs": [ '<!(node -e "require(\'nan\')")' ],
      "conditions": [
        ["OS=='linux'", {
          "defines": [ "_GNU_SOURCE" ],
          "cflags": [ "-g", "-O2", "-std=c++11", ],
        }],
      ],
      "sources": [
        "test_nan_throw.cc"
      ]
    }
  ]
}
