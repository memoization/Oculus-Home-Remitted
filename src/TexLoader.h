#pragma once
#include <fstream>
#include <GLFW/glfw3.h>
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#include <picopng.h>
#include <tuple>
#include <vector>
#include <string>
#include <unordered_map>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <deque>

struct LoadedTexture
{
    GLuint texID = 0;
    int width = 0;
    int height = 0;
    long long mtime = 0; // source-file write time when uploaded, lets async skip unchanged reloads
};

struct TexLoader
{
    bool LoadPng(const std::string& path);
    // Drop any cached texture for path (freeing its GL id) and load it fresh. Needed when a fixed path's file content changes (e.g. profile.png rewritten by a new pick)
    bool Reload(const std::string& path);
    std::tuple<GLuint, int, int> GetPng(const std::string& path);
    void Cleanup();

    // RequestAsync queues a file-read and PNG-decode on a worker thread. Until a texture is uploaded GetPng returns 0, so the caller should draw a placeholder in the meantime
    void RequestAsync(const std::string& path);
    void PumpUploads(int maxPerFrame = 4);

    std::unordered_map<std::string, LoadedTexture> loadedTextures;

private:
    struct Decoded
    {
        std::string path;
        std::vector<unsigned char> pixels;
        int w = 0, h = 0;
        long long mtime = 0;
        bool ok = false;
    };
    void EnsureWorker();
    void WorkerLoop();

    std::thread worker;
    std::mutex asyncMtx;// guards jobs / inFlight / ready / stop
    std::condition_variable cv;
    std::deque<std::string> jobs; // paths awaiting decode (worker input)
    std::unordered_map<std::string, long long> inFlight; // path to mtime currently queued/decoding
    std::deque<Decoded> ready;// decoded, awaiting GL upload

    bool workerStop = false;
    bool workerStarted = false;
};

extern TexLoader texLoader;
