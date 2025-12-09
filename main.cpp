// main.cpp - SentraAI using system curl instead of libcurl
#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <stdexcept>
#include <filesystem>
#include <cmath>
#include <utility>
#include <algorithm>
#include <cstdio>   // popen / _popen
#include <cstdlib>

#include "json.hpp"  // nlohmann::json (json.hpp in the same folder)

using json = nlohmann::json;
namespace fs = std::filesystem;
bool DEBUG_CHAT = false;
// ---------------------- Config & Types ----------------------

struct SentraConfig {
    std::string apiKey;
    std::string baseUrl = "https://api.openai.com/v1";
    std::string embeddingModel = "text-embedding-3-small";
    std::string chatModel      = "gpt-5-nano";

    std::string dataDir      = "data";
    std::string artifactsDir = "artifacts";
    std::string indexPath    = "artifacts/index.bin";
    std::string metaPath     = "artifacts/metadata.json";

    int topK = 3;
};

struct Document {
    std::string id;
    std::string sourcePath;
    std::string content;
};

struct IndexEntry {
    Document doc;
    std::vector<float> embedding;
};

// ---------------------- Small utilities ----------------------

std::string readFileToString(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("Failed to open file: " + path);
    }
    std::string data((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
    return data;
}

void writeStringToFile(const std::string& path, const std::string& data) {
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        throw std::runtime_error("Failed to write file: " + path);
    }
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
}

// run a shell command and capture stdout
std::string runCommand(const std::string& cmd) {
#ifdef _WIN32
    FILE* pipe = _popen(cmd.c_str(), "r");
#else
    FILE* pipe = popen(cmd.c_str(), "r");
#endif
    if (!pipe) {
        throw std::runtime_error("Failed to run command: " + cmd);
    }

    char buffer[4096];
    std::string result;
    while (fgets(buffer, sizeof(buffer), pipe)) {
        result += buffer;
    }

#ifdef _WIN32
    int rc = _pclose(pipe);
#else
    int rc = pclose(pipe);
#endif
    (void)rc; // ignore exit code for now

    return result;
}

// ---------------------- "HTTP client" using system curl ----------------------

class HttpClient {
public:
    explicit HttpClient(const SentraConfig& cfg) : cfg_(cfg) {
        fs::create_directories(cfg_.artifactsDir);
    }

    std::string postJson(const std::string& path, const std::string& bodyJson) {
        // write bodyJson to a temp file
        std::string tmpPath = cfg_.artifactsDir + "/curl_body.json";
        writeStringToFile(tmpPath, bodyJson);

        std::string url = cfg_.baseUrl + path;

        // build curl command
        // -s = silent
        // --fail = non-2xx considered error, but we ignore rc for simplicity and let JSON parsing fail if needed
        std::string cmd = "curl -s -X POST \"" + url + "\" "
                          "-H \"Authorization: Bearer " + cfg_.apiKey + "\" "
                          "-H \"Content-Type: application/json\" "
                          "--data-binary @" + tmpPath;

        std::string response = runCommand(cmd);
        if (response.empty()) {
            throw std::runtime_error("Empty response from curl (or curl failed). Command: " + cmd);
        }
        return response;
    }

private:
    SentraConfig cfg_;
};

// ---------------------- LLM Client (embeddings + chat completions) ----------------------

class LlmClient {
public:
    LlmClient(const SentraConfig& cfg, HttpClient& http)
        : cfg_(cfg), http_(http) {}

    std::vector<float> embed(const std::string& text) {
        json body;
        body["model"] = cfg_.embeddingModel;
        body["input"] = text;

        std::string respStr = http_.postJson("/embeddings", body.dump());
        json resp = json::parse(respStr);

        const auto& embArr = resp["data"][0]["embedding"];
        std::vector<float> embedding;
        embedding.reserve(embArr.size());
        for (auto& v : embArr) {
            embedding.push_back(static_cast<float>(v.get<double>()));
        }
        return embedding;
    }

    std::string chatWithContext(const std::string& question,
                                const std::vector<std::string>& contextChunks) {
        std::string contextText;
        for (const auto& c : contextChunks) {
            contextText += c;
            contextText += "\n\n---\n\n";
        }

        std::string prompt =
            "You are SentraAI, a retrieval-augmented assistant. "
            "Use the provided context when it is relevant to the user's question. "
            "If the question is generic small talk (like 'hello'), you may respond normally. "
            "If the user asks about specific facts not in the context, say you don't know.\n\n"
            "Context:\n" + contextText + "\nQuestion:\n" + question + "\n\nAnswer:";

        json body;
        body["model"] = cfg_.chatModel;
        body["messages"] = json::array({
            json{{"role", "system"}, {"content", "You are a helpful assistant."}},
            json{{"role", "user"},   {"content", prompt}}
        });

        std::string respStr = http_.postJson("/chat/completions", body.dump());
        json resp = json::parse(respStr);
        // DEBUG: print raw response once
        if(DEBUG_CHAT) {
            std::cerr << "Chat raw response:\n" << resp.dump(2) << "\n";
        }


        std::string answer =
            resp["choices"][0]["message"]["content"].get<std::string>();
        return answer;
    }

private:
    SentraConfig cfg_;
    HttpClient& http_;
};

// ---------------------- Vector Index (storage + cosine search) ----------------------

class VectorIndex {
public:
    explicit VectorIndex(const SentraConfig& cfg) : cfg_(cfg) {}

    bool existsOnDisk() const {
        return fs::exists(cfg_.indexPath) && fs::exists(cfg_.metaPath);
    }

    void build(const std::vector<Document>& docs,
                std::vector<std::vector<float>>&& embeddings) {
        if (docs.empty()) {
            throw std::runtime_error("No documents to build index.");
        }
        if (docs.size() != embeddings.size()) {
            throw std::runtime_error("Docs and embeddings size mismatch");
        }

        // Determine the reference embedding dimension from the first non-empty one
        std::size_t refDim = 0;
        for (const auto& emb : embeddings) {
            if (!emb.empty()) {
                refDim = emb.size();
                break;
            }
        }
        if (refDim == 0) {
            throw std::runtime_error("All embeddings are empty.");
        }

        entries_.clear();
        entries_.reserve(docs.size());

        for (size_t i = 0; i < docs.size(); ++i) {
            auto& emb = embeddings[i];

            if (emb.empty()) {
                std::cerr << "[WARN] Skipping doc " << docs[i].id
                        << " because embedding is empty.\n";
                continue;
            }

            if (emb.size() != refDim) {
                std::cerr << "[WARN] Skipping doc " << docs[i].id
                        << " due to embedding dimension mismatch: "
                        << emb.size() << " vs " << refDim << "\n";
                continue;
            }

            IndexEntry e;
            e.doc = docs[i];
            e.embedding = std::move(emb);
            entries_.push_back(std::move(e));
        }

        if (entries_.empty()) {
            throw std::runtime_error("No valid entries after filtering embeddings.");
        }
    }


    void saveToDisk() const {
        if (entries_.empty()) {
            throw std::runtime_error("No entries to save.");
        }

        fs::create_directories(cfg_.artifactsDir);

        // Save embeddings as binary
        std::ofstream out(cfg_.indexPath, std::ios::binary);
        if (!out) {
            throw std::runtime_error("Failed to open index file for writing");
        }

        uint32_t num = static_cast<uint32_t>(entries_.size());
        uint32_t dim = static_cast<uint32_t>(entries_[0].embedding.size());

        out.write(reinterpret_cast<const char*>(&num), sizeof(num));
        out.write(reinterpret_cast<const char*>(&dim), sizeof(dim));

        for (const auto& e : entries_) {
            out.write(reinterpret_cast<const char*>(e.embedding.data()),
                        static_cast<std::streamsize>(dim * sizeof(float)));
        }

        // Save metadata as JSON
        json j = json::array();
        for (const auto& e : entries_) {
            j.push_back({
                {"id",      e.doc.id},
                {"source",  e.doc.sourcePath},
                {"content", e.doc.content}
            });
        }
        writeStringToFile(cfg_.metaPath, j.dump(2));
    }

    void loadFromDisk() {
        entries_.clear();

        std::ifstream in(cfg_.indexPath, std::ios::binary);
        if (!in) {
            throw std::runtime_error("Failed to open index file");
        }

        uint32_t num = 0;
        uint32_t dim = 0;
        in.read(reinterpret_cast<char*>(&num), sizeof(num));
        in.read(reinterpret_cast<char*>(&dim), sizeof(dim));

        std::string metaStr = readFileToString(cfg_.metaPath);
        json j = json::parse(metaStr);

        if (j.size() != num) {
            throw std::runtime_error("Metadata size does not match index");
        }

        entries_.resize(num);
        for (uint32_t i = 0; i < num; ++i) {
            Document d;
            d.id         = j[i]["id"].get<std::string>();
            d.sourcePath = j[i]["source"].get<std::string>();
            d.content    = j[i]["content"].get<std::string>();

            std::vector<float> emb(dim);
            in.read(reinterpret_cast<char*>(emb.data()),
                    static_cast<std::streamsize>(dim * sizeof(float)));

            entries_[i].doc = std::move(d);
            entries_[i].embedding = std::move(emb);
        }
    }

    std::vector<Document> search(const std::vector<float>& queryEmbedding,
                                 int topK) const {
        if (entries_.empty()) {
            throw std::runtime_error("Index is empty.");
        }

        std::vector<std::pair<float, size_t>> scored;
        scored.reserve(entries_.size());
        for (size_t i = 0; i < entries_.size(); ++i) {
            float s = cosineSim(queryEmbedding, entries_[i].embedding);
            scored.emplace_back(s, i);
        }

        if (topK > static_cast<int>(scored.size())) {
            topK = static_cast<int>(scored.size());
        }

        std::partial_sort(
            scored.begin(),
            scored.begin() + topK,
            scored.end(),
            [](const auto& a, const auto& b) {
                return a.first > b.first; // descending
            }
        );

        std::vector<Document> result;
        result.reserve(topK);
        for (int i = 0; i < topK; ++i) {
            size_t idx = scored[i].second;
            result.push_back(entries_[idx].doc);
        }
        return result;
    }

private:
    SentraConfig cfg_;
    std::vector<IndexEntry> entries_;

    static float cosineSim(const std::vector<float>& a,
                           const std::vector<float>& b) {
        if (a.size() != b.size() || a.empty()) {
            return 0.0f;
        }
        double dot = 0.0;
        double na = 0.0;
        double nb = 0.0;
        for (size_t i = 0; i < a.size(); ++i) {
            dot += static_cast<double>(a[i]) * static_cast<double>(b[i]);
            na  += static_cast<double>(a[i]) * static_cast<double>(a[i]);
            nb  += static_cast<double>(b[i]) * static_cast<double>(b[i]);
        }
        if (na == 0.0 || nb == 0.0) {
            return 0.0f;
        }
        return static_cast<float>(dot / (std::sqrt(na) * std::sqrt(nb)));
    }
};

// ---------------------- Engine (orchestration) ----------------------

std::vector<Document> loadDocuments(const std::string& dataDir) {
    std::vector<Document> docs;
    fs::path dirPath(dataDir);

    if (!fs::exists(dirPath)) {
        throw std::runtime_error("Data directory does not exist: " + dataDir);
    }

    int docCounter = 0;
    for (const auto& entry : fs::directory_iterator(dirPath)) {
        if (!entry.is_regular_file()) continue;
        auto path = entry.path();
        if (path.extension() != ".txt") continue;

        std::string content = readFileToString(path.string());
        if (content.empty()) continue;

        // Simple chunking: split by double newline
        std::string::size_type pos = 0;
        while (pos < content.size()) {
            auto next = content.find("\n\n", pos);
            std::string chunk = (next == std::string::npos)
                                    ? content.substr(pos)
                                    : content.substr(pos, next - pos);
            if (!chunk.empty()) {
                Document d;
                d.id = "doc-" + std::to_string(docCounter++);
                d.sourcePath = path.string();
                d.content = chunk;
                docs.push_back(std::move(d));
            }
            if (next == std::string::npos) break;
            pos = next + 2;
        }
    }

    return docs;
}

class SentraEngine {
public:
    SentraEngine(const SentraConfig& cfg, LlmClient& llm, VectorIndex& index)
        : cfg_(cfg), llm_(llm), index_(index) {}

    void buildOrLoadIndex() {
        if (index_.existsOnDisk()) {
            index_.loadFromDisk();
            return;
        }

        auto docs = loadDocuments(cfg_.dataDir);
        if (docs.empty()) {
            throw std::runtime_error("No documents found in data directory.");
        }

        std::vector<std::vector<float>> embeddings;
        embeddings.reserve(docs.size());
        for (const auto& d : docs) {
            embeddings.push_back(llm_.embed(d.content));
        }

        index_.build(docs, std::move(embeddings));
        index_.saveToDisk();
    }

    std::string answer(const std::string& question) {
        // 1) Embed the question
        auto qEmb = llm_.embed(question);

        // 2) Retrieve top-k docs
        auto docs = index_.search(qEmb, cfg_.topK);

        // 3) Build *bounded* context
        std::vector<std::string> context;
        context.reserve(docs.size());

        const std::size_t MAX_TOTAL_CHARS = 3000;   // overall cap ~750 tokens
        const std::size_t MAX_CHARS_PER_CHUNK = 800;
        std::size_t total_chars = 0;

        for (const auto& d : docs) {
            if (total_chars >= MAX_TOTAL_CHARS) break;

            std::string chunk = d.content;

            // per-chunk cap
            if (chunk.size() > MAX_CHARS_PER_CHUNK) {
                chunk = chunk.substr(0, MAX_CHARS_PER_CHUNK);
                chunk += "...";
            }

            // would this push us over the global cap?
            if (total_chars + chunk.size() > MAX_TOTAL_CHARS) {
                std::size_t remaining = MAX_TOTAL_CHARS - total_chars;
                if (remaining > 0 && remaining < chunk.size()) {
                    chunk = chunk.substr(0, remaining);
                    chunk += "...";
                } else if (remaining == 0) {
                    break;
                }
            }

            std::string decorated = "[" + d.sourcePath + "]\n" + chunk;
            total_chars += decorated.size();
            context.push_back(std::move(decorated));
        }

        // Debug: see how big our context really is
        if (DEBUG_CHAT) {
            std::cerr << "Using " << context.size()
                        << " chunks, total_chars=" << total_chars << "\n";
        }

        // 4) Ask LLM with trimmed context
        return llm_.chatWithContext(question, context);
    }


private:
    SentraConfig cfg_;
    LlmClient& llm_;
    VectorIndex& index_;
};

// ---------------------- Config Loader ----------------------

SentraConfig loadConfig() {
    SentraConfig cfg;

    // load API key from api_key.txt (single line)
    std::ifstream in("api_key.txt");
    if (!in) {
        throw std::runtime_error("Create api_key.txt with your API key.");
    }
    std::getline(in, cfg.apiKey);
    if (cfg.apiKey.empty()) {
        throw std::runtime_error("api_key.txt is empty.");
    }

    return cfg;
}

// ---------------------- main() ----------------------

int main() {
    try {
        SentraConfig cfg = loadConfig();
        HttpClient http(cfg);
        LlmClient llm(cfg, http);
        VectorIndex index(cfg);
        SentraEngine engine(cfg, llm, index);

        std::cout << "Building / loading index...\n";
        engine.buildOrLoadIndex();
        std::cout << "SentraAI CLI ready. Type 'exit' to quit.\n\n";

        std::string line;
        while (true) {
            std::cout << "You> ";
            if (!std::getline(std::cin, line)) break;
            if (line == "exit" || line == "quit") break;
            if (line.empty()) continue;

            try {
                std::string ans = engine.answer(line);
                for (char& c : ans) {
                    if (static_cast<unsigned char>(c) == 0x92 ||
                    static_cast<unsigned char>(c) == 0x27) {
                        c = '\'';
                    }
                }
                std::cout << "\nSentraAI> " << ans << "\n\n";
            } catch (const std::exception& ex) {
                std::cerr << "Error: " << ex.what() << "\n";
            }
        }

        std::cout << "Bye.\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "Fatal error: " << ex.what() << "\n";
        return 1;
    }
}
