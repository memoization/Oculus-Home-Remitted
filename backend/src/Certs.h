#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wincrypt.h>
#include <string>

// A root CA and a leaf CN=graph.oculus.com signed by it, generated at runtime via CryptoAPI. 
// No embedded secret and the private keys never leave the process and are never logged.
namespace home2backend {

bool GenerateCerts();
PCCERT_CONTEXT GetLeafCert(); // presented by the TLS server (has the private key)
PCCERT_CONTEXT GetCaCert();  // injected into the game's ROOT trust set

// Write the CA public cert (no private key) to a PEM file
bool ExportCaPem(const std::wstring& path);

}
