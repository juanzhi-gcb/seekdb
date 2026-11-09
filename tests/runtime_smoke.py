"""Verify the final runtime image across independent container lifetimes."""
import pathlib
import subprocess
import sys
import tempfile

image = sys.argv[1] if len(sys.argv) > 1 else "minivectordb:dev"
root = pathlib.Path(sys.argv[2] if len(sys.argv) > 2 else "/data/runtime-tests").resolve()
root.mkdir(parents=True, exist_ok=True)
with tempfile.TemporaryDirectory(prefix="runtime-", dir=root) as directory:
    args = ["docker", "--config", "/var/lib/minivectordb-docker-client",
            "run", "--rm", "--read-only", "--network", "none", "--cpus", "2",
            "--memory", "512m", "-i", "--mount", f"type=bind,src={directory},dst=/data", image]
    def sql(text):
        result = subprocess.run(args, input=text, text=True, capture_output=True, timeout=45)
        assert result.returncode == 0, result.stdout + result.stderr
        return result.stdout.splitlines()
    first = sql("""
        CREATE TABLE t(id BIGINT PRIMARY KEY,label VARCHAR(20),v VECTOR(2));
        BEGIN;
        INSERT INTO t VALUES(1,'durable',[3,4]),(2,'second',[0,0]);
        COMMIT;
        BEGIN; DELETE FROM t WHERE id=1;
    """)
    assert first == ["OK\t0", "OK\t0", "OK\t2", "OK\t0", "OK\t0", "OK\t1"], first
    second = sql("SELECT id,label,L2_DISTANCE(v,[0,0]) AS d FROM t ORDER BY d;")
    assert second == ["RESULT\tid\tlabel\td", "ROW\ti:2\ts:second\td:0",
                      "ROW\ti:1\ts:durable\td:5", "END\t2"], second
    assert (pathlib.Path(directory) / "database.mvdb").is_file()
    print("PASS runtime: read-only root, network disabled, 2 CPU/512 MiB, /data persistence and EOF rollback")