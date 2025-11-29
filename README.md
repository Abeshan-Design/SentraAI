# SentraAI — C++ Retrieval-Augmented Generation (RAG) System

SentraAI is a high-performance Retrieval-Augmented Generation system built primarily in C++, with a lightweight Python ingestion layer for PDF/text preprocessing.
It embeds your documents, stores vectors in a custom binary index, retrieves the top-k matches, and generates grounded answers using GPT-5 Nano.

This project emphasizes systems-level C++ work, clean architecture, and efficient interoperability between C++ and Python.

## Features
### C++ Core Engine
- Custom vector index (binary on-disk format)
- Cosine similarity retrieval
- Memory-efficient chunked context building
- JSON handling with nlohmann/json.hpp
- Direct raw HTTP requests using curl
- CLI chatbot interface
- Python Ingestion Layer
- Converts PDFs → cleaned .txt
- Simple + dependency-light (PyPDF only)
- RAG Behavior
- Embeds documents using text-embedding-3-small
- Stores vectors + metadata in /artifacts
- Retrieves top-k chunks per query
- Uses gpt-5-nano for grounded answers


## Architecture Overview
                 +----------------+
                 |     User (CLI) |
                 +--------+-------+
                          |
                          v
                 +----------------------+
                 |    SentraEngine     |
                 |      (C++ RAG)      |
                 +-----+---------+-----+
                       |         |
             Embedding |         | Retrieval
                       |         |
                       v         v
           +----------------+   +----------------+
           | OpenAI         |   | VectorIndex    |
           | Embeddings API |   | (binary store) |
           +--------+-------+   +--------+-------+
                    \               /
                     \             /
                      \           /
                       +---------+
                       | Context |
                       +---------+
                           |
                           v
               +---------------------------+
               | GPT-5 Nano (Chat Completion) |
               +---------------------------+



## Installation & Setup
### 1. Install dependencies
Python:
```
pip install pypdf
```
C++ (bundled dependencies):
- g++ (C++17 or later)
- curl
- nlohmann/json.hpp (already included)

### 2. Add your OpenAI API key
Create:
```
api_key.txt
```
Put only your API key inside.
> ⚠️ This file is ignored by Git. Never commit it.

### 3. Add documents
Place PDFs or .txt files in:
```
data_raw/
```

### 4. Run ingestion
```
python ingest_pdfs.py
```
This converts PDFs → cleaned .txt files stored in data/.

### 5. Build SentraAI (C++)
Windows:
```
g++ -std=c++17 main.cpp -o sentra.exe -lcurl
```

Linux/macOS:
```
g++ -std=c++17 main.cpp -o sentra -lcurl
```

### 6. Run the CLI chatbot
```
./sentra
```
Example:
```
You> What does this document discuss?
SentraAI> The text argues that...
```

## Running the Web Interface
1. **Install Python dependencies**
```
cd frontend
pip install -r requirements.txt
```

2. **Start the FastAPI server**:
```
python3 -m uvicorn app:app --reload --host 0.0.0.0 --port 8000
```

3. **Open your browser**:
Enter: `http://localhost:8000`

### Using the Web Interface
- **View Documents**: Click "View Documents" to see all indexed documents
- **Re-ingest PDFs**: Click "Re-ingest PDFs" to reprocess files in 'data_raw/' and rebuild index

### Troubleshoot
**Use this port if 8000 does not work**
```
python3 -m uvicorn app:app --reload --host 0.0.0.0 --port 8001
```

## Model Pricing (GPT-5 Nano)
| Model              | Input (per 1K tokens) | Output (per 1K tokens) |
|--------------------|------------------------|--------------------------|
| gpt-5-nano         | $0.0002                | $0.0004                  |
| gpt-5-nano-high    | $0.0003                | $0.0006                  |

Typical SentraAI query cost: ≈ $0.0003 (1/30th of a cent).

## Security Notes
- api_key.txt is ignored — do not commit it.
- data/, data_raw/, artifacts/, and build files are ignored.

## Future Enhancements
- Add streaming responses
- Multi-threaded index build
- FastAPI web frontend
- Hybrid scoring (semantic + keyword)
- Vector index inspection tool

## Author
Developed by Abeshan, Khalon
A high-performance C++ RAG system demonstrating systems engineering, AI integration, and cross-language tooling.
