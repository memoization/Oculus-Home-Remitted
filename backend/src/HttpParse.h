#pragma once
#include <string>
#include <cstddef>

namespace home2hook {

// True if the buffer begins with an HTTP request-line method token ("POST ", "GET ", ...). Used to detect the first write of a new request on a keep-alive connection
bool StartsWithHttpMethod(const char* data, size_t length);

// Parse an accumulated request buffer. Returns true only for a /graphql POST whose form body already contains a doc_id, filling docId and variablesJson, both url-decoded, with variablesJson empty if the field is absent.
bool ParseGraphqlRequest(const std::string& request, std::string& docId, std::string& variablesJson);

std::string UrlDecode(const std::string& text);

// Parse a multipart/form-data POST (world_upload_screenshot / world_upload_cubemap). Reads the boundary from the Content-Type header, extracts the "world_id" field value and the raw bytes of the "file" part, with its filename extension into extOut.
// Returns true only when both a world_id and file part were found. The file bytes are binary and must not be logged.
bool ParseMultipartUpload(const std::string& request, std::string& worldIdOut, std::string& fileBytesOut, std::string& extOut);

}
