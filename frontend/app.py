from fastapi import FastAPI, HTTPException, Request
from fastapi.middleware.cors import CORSMiddleware
from fastapi.staticfiles import StaticFiles
from fastapi.templating import Jinja2Templates
from pydantic import BaseModel
import subprocess
import json
from pathlib import Path
from typing import List, Optional
import os

app = FastAPI(title="SentraAI", version="1.0.0")

# Paths relative to frontend/
BASE_DIR = Path(__file__).parent
BACKEND_DIR = BASE_DIR.parent
SENTRA_BIN = BACKEND_DIR / "sentra"
ARTIFACTS_DIR = BACKEND_DIR / "artifacts"
DATA_DIR = BACKEND_DIR / "data"

# Mount static files and templates
app.mount("/static", StaticFiles(directory=BASE_DIR / "static"), name="static")
templates = Jinja2Templates(directory=BASE_DIR / "templates")

# Enable CORS
app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_credentials=True,
    allow_methods=["*"],
    allow_headers=["*"],
)

# --- Models ---

class QueryRequest(BaseModel):
    question: str

class QueryResponse(BaseModel):
    answer: str
    sources: Optional[List[str]] = None

class DocumentInfo(BaseModel):
    id: str
    source: str
    content_preview: str

# --- Helper Functions ---

def query_sentra(question: str) -> str:
    """Call the C++ binary and get response"""
    try:
        result = subprocess.run(
            [str(SENTRA_BIN)],
            input=f"{question}\nexit\n",
            capture_output=True,
            text=True,
            timeout=30,
            cwd=str(BACKEND_DIR)
        )

        output = result.stdout
        if "SentraAI>" in output:
            # Extract answer between "SentraAI>" and next "\n\n"
            answer = output.split("SentraAI>")[1].split("\n\n")[0].strip()
            return answer
        return "No response from engine"

    except subprocess.TimeoutExpired:
        raise HTTPException(status_code=504, detail="Query timeout")
    except FileNotFoundError:
        raise HTTPException(status_code=500, detail="Sentra binary not found. Run: g++ -std=c++17 main.cpp -o sentra")
    except Exception as e:
        raise HTTPException(status_code=500, detail=str(e))

# --- Routes ---

@app.get("/")
async def home(request: Request):
    """Serve the main UI"""
    return templates.TemplateResponse("index.html", {"request": request})

@app.post("/api/query", response_model=QueryResponse)
async def query(req: QueryRequest):
    """Ask a question to SentraAI"""
    if not req.question.strip():
        raise HTTPException(status_code=400, detail="Question cannot be empty")

    answer = query_sentra(req.question)
    return QueryResponse(answer=answer)

@app.get("/api/documents", response_model=List[DocumentInfo])
async def list_documents(limit: int = 50):
    """List indexed documents"""
    meta_path = ARTIFACTS_DIR / "metadata.json"

    if not meta_path.exists():
        raise HTTPException(status_code=404, detail="Index not built. Run ./sentra first.")

    try:
        with open(meta_path, 'r', encoding='utf-8') as f:
            data = json.load(f)

        docs = []
        for item in data[:limit]:
            docs.append(DocumentInfo(
                id=item['id'],
                source=item['source'],
                content_preview=item['content'][:200] + "..." if len(item['content']) > 200 else item['content']
            ))
        return docs

    except Exception as e:
        raise HTTPException(status_code=500, detail=f"Error reading metadata: {str(e)}")

@app.post("/api/ingest")
async def trigger_ingest():
    """Run PDF ingestion and rebuild index"""
    try:
        # Run ingestion script
        result = subprocess.run(
            ['python', 'ingest_pdfs.py'],
            cwd=str(BACKEND_DIR),
            capture_output=True,
            text=True,
            check=True
        )

        # Delete old index to force rebuild
        (ARTIFACTS_DIR / "index.bin").unlink(missing_ok=True)
        (ARTIFACTS_DIR / "metadata.json").unlink(missing_ok=True)

        return {
            "status": "success",
            "message": "Ingestion complete. Index will rebuild on next query.",
            "output": result.stdout
        }
    except subprocess.CalledProcessError as e:
        raise HTTPException(status_code=500, detail=f"Ingestion failed: {e.stderr}")
    except Exception as e:
        raise HTTPException(status_code=500, detail=str(e))

@app.get("/api/health")
async def health_check():
    """System health check"""
    index_exists = (ARTIFACTS_DIR / "index.bin").exists()
    meta_exists = (ARTIFACTS_DIR / "metadata.json").exists()
    sentra_exists = SENTRA_BIN.exists()

    return {
        "status": "healthy" if sentra_exists else "error",
        "index_loaded": index_exists and meta_exists,
        "binary_found": sentra_exists,
        "data_dir": str(DATA_DIR),
        "artifacts_dir": str(ARTIFACTS_DIR)
    }

@app.get("/api/stats")
async def get_stats():
    """Get system statistics"""
    meta_path = ARTIFACTS_DIR / "metadata.json"

    doc_count = 0
    if meta_path.exists():
        try:
            with open(meta_path, 'r') as f:
                data = json.load(f)
                doc_count = len(data)
        except:
            pass

    return {
        "total_documents": doc_count,
        "index_size_mb": round((ARTIFACTS_DIR / "index.bin").stat().st_size / (1024*1024), 2) if (ARTIFACTS_DIR / "index.bin").exists() else 0,
        "data_files": len(list(DATA_DIR.glob("*.txt"))) if DATA_DIR.exists() else 0
    }

if __name__ == "__main__":
    import uvicorn
    uvicorn.run(app, host="0.0.0.0", port=8000, reload=True)