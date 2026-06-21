usage: wayland-scanner [OPTION] [client-header|server-header|enum-header|private-code|public-code] [input_file output_file]

Converts XML protocol descriptions supplied on stdin or input file to client
headers, server headers, or protocol marshalling code.

Use "public-code" only if the marshalling code will be public - aka DSO will export it while other components will be using it.
Using "private-code" is strongly recommended.

options:
    -h,  --help                  display this help and exit.
    -v,  --version               print the wayland library version that
                                 the scanner was built against.
    -c,  --include-core-only     include the core version of the headers,
                                 that is e.g. wayland-client-core.h instead
                                 of wayland-client.h.
    -s,  --strict                exit immediately with an error if DTD
                                 verification fails.
