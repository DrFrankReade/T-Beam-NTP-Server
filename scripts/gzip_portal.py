from pathlib import Path
import gzip
import io

Import("env")


def gzip_bytes(data):
    output = io.BytesIO()
    with gzip.GzipFile(fileobj=output, mode="wb", compresslevel=9, mtime=0) as gz_file:
        gz_file.write(data)
    return output.getvalue()


project_dir = Path(env.subst("$PROJECT_DIR"))
source = project_dir / "data" / "index.html"
target = project_dir / "data" / "index.html.gz"

if source.exists():
    compressed = gzip_bytes(source.read_bytes())
    if not target.exists() or target.read_bytes() != compressed:
        target.write_bytes(compressed)
