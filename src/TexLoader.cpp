#include "TexLoader.h"
#include "HomeLogger.h"
#include <filesystem>

TexLoader texLoader;

// File write-time as a comparable integer (0 if missing). Implementation-defined epoch, but consistent within a run, which is all the async change-detection needs.
static long long FileMtime(const std::string& path)
{
    std::error_code ec;
    auto t = std::filesystem::last_write_time(path, ec);
    
    if (ec) return 0;
    return static_cast<long long>(t.time_since_epoch().count());
}

bool TexLoader::LoadPng(const std::string& path)
{
    if (texLoader.loadedTextures.contains(path))
    {
        return true;
    }

    homeLogger.write() << "Loading image: " << path.c_str() << std::endl;

    // Read file into buffer
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file)
    {
        homeLogger.write() << "Failed to load image: not found." << std::endl;
        texLoader.loadedTextures[path] = LoadedTexture{};
        return false;
    }

    std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);

    std::vector<unsigned char> buffer(size);
    if (!file.read((char*)buffer.data(), size))
    {
        homeLogger.write() << "Failed to load image: unable to read file." << std::endl;

        texLoader.loadedTextures[path] = LoadedTexture{};
        return false;
    }

    // Decode PNG
    std::vector<unsigned char> decoded;
    unsigned long w, h;
    if (decodePNG(decoded, w, h, buffer.data(), buffer.size(), true) != 0)
    {
        homeLogger.write() << "Failed to load image: unable to decode." << std::endl;
        texLoader.loadedTextures[path] = LoadedTexture{};
        return false;
    }

    // Create OpenGL texture
    GLuint texID;
    glGenTextures(1, &texID);
    glBindTexture(GL_TEXTURE_2D, texID);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);

    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8,
        (GLsizei)w, (GLsizei)h,
        0, GL_RGBA, GL_UNSIGNED_BYTE, decoded.data());

    LoadedTexture tex;
    tex.texID = texID;
    tex.width = (int)w;
    tex.height = (int)h;
    tex.mtime = FileMtime(path);

    texLoader.loadedTextures[path] = tex;
    return true;
}

bool TexLoader::Reload(const std::string& path)
{
    auto it = texLoader.loadedTextures.find(path);
    if (it != texLoader.loadedTextures.end())
    {
        if (it->second.texID)
            glDeleteTextures(1, &it->second.texID);
        texLoader.loadedTextures.erase(it);
    }
    return LoadPng(path);
}

std::tuple<GLuint, int, int> TexLoader::GetPng(const std::string& path)
{
    auto it = texLoader.loadedTextures.find(path);
    if (it != texLoader.loadedTextures.end())
    {
        return { it->second.texID, it->second.width, it->second.height };
    }

    return { 0, 0, 0 };
}

// ---- Async decode (worker thread) and render-thread GL upload ----

void TexLoader::EnsureWorker()
{
    if (workerStarted) return;

    workerStarted = true;
    worker = std::thread([this] { WorkerLoop(); });
}

void TexLoader::WorkerLoop()
{
    for (;;)
    {
        std::string path;
        {
            std::unique_lock<std::mutex> lk(asyncMtx);
            cv.wait(lk, [this] { return workerStop || !jobs.empty(); });
            if (workerStop && jobs.empty()) return;

            path = std::move(jobs.front());
            jobs.pop_front();
        }

        // No GL calls here, pure file read and decode, safe off the render thread.
        Decoded d;
        d.path = path;
        d.mtime = FileMtime(path);
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (file)
        {
            std::streamsize size = file.tellg();
            file.seekg(0, std::ios::beg);
            std::vector<unsigned char> buffer((size_t)size);
            if (file.read((char*)buffer.data(), size))
            {
                unsigned long w = 0, h = 0;
                if (decodePNG(d.pixels, w, h, buffer.data(), buffer.size(), true) == 0)
                {
                    d.w = (int)w;
                    d.h = (int)h;
                    d.ok = true;
                }
            }
        }

        {
            std::lock_guard<std::mutex> lk(asyncMtx);
            ready.push_back(std::move(d));
        }
    }
}

void TexLoader::RequestAsync(const std::string& path)
{
    long long mt = FileMtime(path);
    if (mt == 0) return; // no file, nothing to decode (just use a placeholder)

    // Already uploaded and then unchanged? (loadedTextures is render-thread only, no lock needed)
    auto it = loadedTextures.find(path);
    if (it != loadedTextures.end() && it->second.texID != 0 && it->second.mtime == mt)
        return;

    {
        std::lock_guard<std::mutex> lk(asyncMtx);
        auto f = inFlight.find(path);
        if (f != inFlight.end() && f->second == mt) return; // this version is already queued/decoding

        inFlight[path] = mt;
        jobs.push_back(path);
    }
    EnsureWorker();
    cv.notify_one();
}

void TexLoader::PumpUploads(int maxPerFrame)
{
    for (int n = 0; n < maxPerFrame; ++n)
    {
        Decoded d;
        {
            std::lock_guard<std::mutex> lk(asyncMtx);
            if (ready.empty())
                return;
            d = std::move(ready.front());
            ready.pop_front();
            auto f = inFlight.find(d.path);
            if (f != inFlight.end() && f->second == d.mtime)
                inFlight.erase(f); // decode for this version is now consumed
        }

        // GL work on the render thread. loadedTextures is render-thread only
        if (!d.ok)
        {
            // cache an empty entry so GetPng returns 0 and don't re-request
            loadedTextures[d.path] = LoadedTexture{ 0, 0, 0, d.mtime };
            continue;
        }

        auto it = loadedTextures.find(d.path);
        if (it != loadedTextures.end() && it->second.texID) glDeleteTextures(1, &it->second.texID);

        GLuint texID;
        glGenTextures(1, &texID);
        glBindTexture(GL_TEXTURE_2D, texID);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)d.w, (GLsizei)d.h, 0, GL_RGBA, GL_UNSIGNED_BYTE, d.pixels.data());

        loadedTextures[d.path] = LoadedTexture{ texID, d.w, d.h, d.mtime };
    }
}

void TexLoader::Cleanup()
{
    if (workerStarted)
    {
        {
            std::lock_guard<std::mutex> lk(asyncMtx);
            workerStop = true;
        }
        cv.notify_all();
        if (worker.joinable())
        {
            worker.join();
        }
            
        workerStarted = false;
    }

    for (auto& [_, tex] : loadedTextures)
    {
        if (tex.texID)
        {
            glDeleteTextures(1, &tex.texID);
        }
    }
    loadedTextures.clear();
}
