# Rewrites the generated shell so the browser can never reuse a stale module.
# The asset file names never change between deploys, so without a build stamp
# the HTTP cache happily serves the previous wasmraw.js/wasm pair.
if(NOT DEFINED INPUT OR NOT DEFINED BUILD_ID)
    message(FATAL_ERROR "INPUT and BUILD_ID are required")
endif()

file(READ "${INPUT}" html)

string(REPLACE "src=wasmraw.js" "src=wasmraw.js?build=${BUILD_ID}" html "${html}")
string(REPLACE "src=\"wasmraw.js\"" "src=\"wasmraw.js?build=${BUILD_ID}\"" html "${html}")
string(REPLACE "<head>" "<head><meta name=\"build\" content=\"${BUILD_ID}\">" html "${html}")

file(WRITE "${INPUT}" "${html}")
