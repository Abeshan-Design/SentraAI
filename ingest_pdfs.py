import os
from pathlib import Path

try:
    from pypdf import PdfReader  
except ImportError:
    print("Please install pypdf: pip install pypdf")
    raise SystemExit(1)

RAW_DIR = Path("data_raw")
OUT_DIR = Path("data")

def extract_pdf_to_text(pdf_path: Path, out_path: Path) -> None:
    reader = PdfReader(str(pdf_path))
    texts = []
    for page in reader.pages:
        text = page.extract_text() or ""
        texts.append(text)
    full_text = "\n\n".join(texts)
    out_path.write_text(full_text, encoding="utf-8")
    print(f"[PDF] {pdf_path.name} -> {out_path}")

def copy_txt(raw_path: Path, out_path: Path) -> None:
    text = raw_path.read_text(encoding="utf-8", errors="ignore")
    out_path.write_text(text, encoding="utf-8")
    print(f"[TXT] {raw_path.name} -> {out_path}")

def main() -> None:
    RAW_DIR.mkdir(exist_ok=True)
    OUT_DIR.mkdir(exist_ok=True)

    for path in RAW_DIR.iterdir():
        if not path.is_file():
            continue

        out_path = OUT_DIR / (path.stem + ".txt")

        if path.suffix.lower() == ".pdf":
            extract_pdf_to_text(path, out_path)
        elif path.suffix.lower() in {".txt", ".md"}:
            copy_txt(path, out_path)
        else:
            print(f"[SKIP] Unsupported file type: {path.name}")

if __name__ == "__main__":
    main()
