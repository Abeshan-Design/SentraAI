from fastapi import FastAPI, HTTPException, Request, Response
from fastapi.middleware.cors import CORSMiddleware
from fastapi.staticfiles import StaticFiles
from fastapi.templating import Jinja2Templates
from pydantic import BaseModel
import subprocess
import json
from pathlib import Path
from typing import List, Optional
import os
import time

from prometheus_client import (
    Counter,
    Histogram,
    Gauge,
    generate_latest,
    CONTENT_TYPE_LATEST,
)

app = FastAPI(title="SentraAI", version="1.0.0")

# Paths relative to frontend/
BASE_DIR = Path(__file__).parent
BACKEND_DIR = BASE_DIR.parent
SENTRA_BIN = BACKEND_DIR / "sentra"
ARTIFACTS_DIR = BACKEND_DIR / "artifacts"
DATA_DIR = BACKEND_DIR / "data"

# --- Prometheus metrics ---

REQUEST_COUNTER = Counter(
    "sentra_requests_total",
    "Total SentraAI API requests",
    ["endpoint", "status"],
)

REQUEST_LATENCY = Histogram(
    "sentra_request_duration_seconds",
    "SentraAI request latency in seconds",
    ["endpoint"],
)

INDEX_DOCS = Gauge(
    "sentra_index_documents",
    "Number of indexed document chunks",
)

INDEX_SIZE_BYTES = Gauge(
    "sentra_index_size_bytes",
    "Index size on disk (bytes)",
)

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
            cwd=str(BACKEND_DIR),
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
        raise HTTPException(
            status_code=500,
            detail="Sentra binary not found. Run: g++ -std=c++17 main.cpp -o sentra",
        )
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
    endpoint = "/api/query"
    start = time.perf_counter()
    status_label = "error"

    try:
        if not req.question.strip():
            raise HTTPException(status_code=400, detail="Question cannot be empty")

        answer = query_sentra(req.question)
        status_label = "success"
        return QueryResponse(answer=answer)

    except HTTPException:
        # still record metrics, then re-raise
        raise
    except Exception as e:
        raise HTTPException(status_code=500, detail=str(e))
    finally:
        duration = time.perf_counter() - start
        REQUEST_LATENCY.labels(endpoint=endpoint).observe(duration)
        REQUEST_COUNTER.labels(endpoint=endpoint, status=status_label).inc()


@app.get("/api/documents", response_model=List[DocumentInfo])
async def list_documents(limit: int = 50):
    """List indexed documents"""
    meta_path = ARTIFACTS_DIR / "metadata.json"

    if not meta_path.exists():
        raise HTTPException(
            status_code=404, detail="Index not built. Run ./sentra first."
        )

    try:
        with open(meta_path, "r", encoding="utf-8") as f:
            data = json.load(f)

        docs = []
        # If metadata is a dict like {"documents": [...]}, normalize
        if isinstance(data, dict) and "documents" in data:
            items = data["documents"]
        else:
            items = data

        for item in items[:limit]:
            content = item.get("content", "")
            docs.append(
                DocumentInfo(
                    id=str(item.get("id", "")),
                    source=item.get("source", ""),
                    content_preview=(
                        content[:200] + "..." if len(content) > 200 else content
                    ),
                )
            )
        return docs

    except Exception as e:
        raise HTTPException(status_code=500, detail=f"Error reading metadata: {str(e)}")


@app.post("/api/ingest")
async def trigger_ingest():
    """Run PDF ingestion and rebuild index"""
    endpoint = "/api/ingest"
    start = time.perf_counter()
    status_label = "error"

    try:
        # Run ingestion script
        result = subprocess.run(
            ["python", "ingest_pdfs.py"],
            cwd=str(BACKEND_DIR),
            capture_output=True,
            text=True,
            check=True,
        )

        # Delete old index to force rebuild
        (ARTIFACTS_DIR / "index.bin").unlink(missing_ok=True)
        (ARTIFACTS_DIR / "metadata.json").unlink(missing_ok=True)

        status_label = "success"
        return {
            "status": "success",
            "message": "Ingestion complete. Index will rebuild on next query.",
            "output": result.stdout,
        }
    except subprocess.CalledProcessError as e:
        raise HTTPException(status_code=500, detail=f"Ingestion failed: {e.stderr}")
    except Exception as e:
        raise HTTPException(status_code=500, detail=str(e))
    finally:
        duration = time.perf_counter() - start
        REQUEST_LATENCY.labels(endpoint=endpoint).observe(duration)
        REQUEST_COUNTER.labels(endpoint=endpoint, status=status_label).inc()


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
        "artifacts_dir": str(ARTIFACTS_DIR),
    }


@app.get("/api/stats")
async def get_stats():
    """Get system statistics"""
    meta_path = ARTIFACTS_DIR / "metadata.json"

    doc_count = 0
    if meta_path.exists():
        try:
            with open(meta_path, "r", encoding="utf-8") as f:
                data = json.load(f)
                if isinstance(data, dict) and "documents" in data:
                    doc_count = len(data["documents"])
                else:
                    doc_count = len(data)
        except Exception:
            pass

    index_path = ARTIFACTS_DIR / "index.bin"
    index_bytes = index_path.stat().st_size if index_path.exists() else 0

    # update Prometheus gauges
    INDEX_DOCS.set(doc_count)
    INDEX_SIZE_BYTES.set(index_bytes)

    return {
        "total_documents": doc_count,
        "index_size_mb": round(index_bytes / (1024 * 1024), 2)
        if index_bytes > 0
        else 0,
        "data_files": len(list(DATA_DIR.glob("*.txt"))) if DATA_DIR.exists() else 0,
    }


@app.get("/metrics")
async def metrics():
    """Prometheus metrics endpoint"""
    data = generate_latest()
    return Response(content=data, media_type=CONTENT_TYPE_LATEST)


if __name__ == "__main__":
    import uvicorn

    uvicorn.run(app, host="0.0.0.0", port=8000, reload=True)
