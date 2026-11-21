#!/usr/bin/env python3
# ---------------------------------------------------------------------------
# CorridorKey 2 for Resolve - worker  (v1.06)
#
# From Creative Video Tips (CVT)
# Created by Chadwick  |  https://creativevideotips.com
# An unofficial plugin around CorridorKey 2.0 by Corridor Digital.
# Licence: CC BY-NC-SA 4.0 with the CorridorKey additional terms (see LICENSE).
#
# What it does:
#   1. Reads a job file (JSON) written by the plugin: a folder of source
#      frames, optional hint frames, and where the results go.
#   2. Loads CorridorKey 2.0 from its own model folder (unedited) on the
#      Apple GPU, choosing the window size and precision for this Mac.
#   3. Writes one RGBA EXR per frame to the cache (half float, DWAB 45):
#      RGB = the keyed foreground in scene-linear light (no despill),
#      A = the matte.
#   4. Reports progress as JSON lines on stdout (CorridorKey's own messages
#      go to stderr) and in a progress window with Cancel (ck2_progress.py).
#
# Usage:  python ck2_worker.py run   <job.json>
#         python ck2_worker.py plan  <job.json>   (show window/precision only)
#         python ck2_worker.py check <model folder> (load the model once, for Setup)
# ---------------------------------------------------------------------------
from __future__ import annotations

import json
import os
import signal
import re
import shutil
import subprocess
import sys
import time
import traceback
from pathlib import Path

os.environ.setdefault("OPENCV_IO_ENABLE_OPENEXR", "1")   # before any cv2 import

VERSION = "1.06"
WINDOWS = (80, 40, 24, 16, 8, 4, 2, 1)

# Peak memory on the Apple GPU, measured 2026-11-13 on M1/M2/M5 Max at HD
# (model input 1920x1152): about 8 GB plus 0.95 GB per frame in the window.
MEM_BASE_GB = 8.0
MEM_PER_FRAME_GB = 0.95
MEM_REF_PIXELS = 1920 * 1152
MEM_USE_FRACTION = 0.85      # leave room for the host app
_T0 = time.time()


# --- plain helpers (no torch, no Resolve) ----------------------------------

_WINDOW = None                         # the progress window process, if any


class Cancelled(Exception):
    """The progress window's Cancel (or the plugin) stopped the job."""


def _on_sigterm(signum, frame):
    raise Cancelled("Cancelled")


def emit(event: str, **fields) -> None:
    """One JSON line on the real stdout for the plugin (and the window) to read."""
    fields["event"] = event
    fields["t"] = round(time.time() - _T0, 2)
    line = json.dumps(fields) + "\n"
    sys.__stdout__.write(line)
    sys.__stdout__.flush()
    if _WINDOW is not None and _WINDOW.poll() is None:
        try:
            _WINDOW.stdin.write(line)
            _WINDOW.stdin.flush()
        except (BrokenPipeError, OSError, ValueError):
            pass


def open_window(title: str) -> None:
    """Starts the progress window (ck2_progress.py beside this file). Never
    fatal: without a window the job runs the same."""
    global _WINDOW
    if os.environ.get("CK2_NO_WINDOW"):
        return
    try:
        _WINDOW = subprocess.Popen(
            [sys.executable, str(Path(__file__).with_name("ck2_progress.py")), str(os.getpid()), title],
            stdin=subprocess.PIPE, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, text=True)
    except OSError:
        _WINDOW = None


def close_window() -> None:
    if _WINDOW is not None:
        try:
            _WINDOW.stdin.close()            # the window shows the end, then closes itself
        except OSError:
            pass


def estimate_peak_gb(width: int, height: int, window: int) -> float:
    scale = (width * height) / MEM_REF_PIXELS
    return MEM_BASE_GB + MEM_PER_FRAME_GB * window * scale


def padded_size(width: int, height: int, quantum: int = 128) -> tuple[int, int]:
    """CorridorKey pads the picture up to a multiple of 128."""
    up = lambda v: -(-v // quantum) * quantum
    return up(width), up(height)


def choose_window(width: int, height: int, n_frames: int, budget_gb: float) -> int:
    """Largest window whose estimated peak fits the budget (at least 1)."""
    pw, ph = padded_size(width, height)
    for w in WINDOWS:
        eff = min(w, n_frames)
        if estimate_peak_gb(pw, ph, eff) <= budget_gb:
            return eff
    return 1


def chunk_starts(n_frames: int, chunk: int, stride: int) -> list[int]:
    """Where each temporal chunk starts, as CorridorKey lays them out: every
    `stride` frames, plus a last one ending on the final frame."""
    chunk = max(1, min(chunk, n_frames))
    starts = list(range(0, max(1, n_frames - chunk + 1), max(1, stride)))
    if starts[-1] + chunk < n_frames:
        starts.append(n_frames - chunk)
    return starts


def chunk_of_frame(done: int, starts: list[int], chunk: int) -> int:
    """1-based chunk that the frame count `done` falls in (for progress)."""
    for i, st in enumerate(starts):
        if done <= st + chunk:
            return i + 1
    return len(starts)


def chip_generation(brand: str) -> int:
    """'Apple M2 Max' -> 2; 0 when unknown."""
    m = re.search(r"Apple M(\d+)", brand)
    return int(m.group(1)) if m else 0


def choose_precision(brand: str) -> str:
    """bf16 on M5 and later (native support); fp16 is faster on M1-M4."""
    return "bf16" if chip_generation(brand) >= 5 else "fp16"


def mac_brand() -> str:
    try:
        return subprocess.run(["sysctl", "-n", "machdep.cpu.brand_string"],
                              capture_output=True, text=True).stdout.strip()
    except OSError:
        return ""


def available_system_gb() -> float:
    """Free + inactive + purgeable + speculative memory, from vm_stat."""
    try:
        out = subprocess.run(["vm_stat"], capture_output=True, text=True).stdout
    except OSError:
        return float("inf")
    page = int(re.search(r"page size of (\d+)", out).group(1))
    pages = 0
    for key in ("Pages free", "Pages inactive", "Pages purgeable", "Pages speculative"):
        m = re.search(rf"{key}:\s+(\d+)", out)
        if m:
            pages += int(m.group(1))
    return pages * page / 2**30


def list_frames(folder: str) -> list[str]:
    exts = (".exr", ".png", ".tif", ".tiff")
    return sorted(str(p) for p in Path(folder).iterdir()
                  if p.suffix.lower() in exts and not p.name.startswith("."))


def load_job(path: str) -> dict:
    with open(path) as f:
        job = json.load(f)
    job.setdefault("input_linear", False)
    job.setdefault("window", "auto")
    job.setdefault("precision", "auto")
    job.setdefault("hint", {})
    job.setdefault("private", True)        # False: results readable by other users (shared folder)
    job["hint"].setdefault("mode", "none")
    job["hint"].setdefault("strength", 0.5)   # low: CorridorKey keeps its own edges
    job["hint"].setdefault("carry", True)
    for key in ("model_dir", "frames_dir", "output_dir", "output_names"):
        if key not in job:
            raise ValueError(f"job file has no '{key}'")
    job["model_dir"] = os.path.expanduser(job["model_dir"])
    return job


def frame_size(path: str) -> tuple[int, int]:
    import cv2
    img = cv2.imread(path, cv2.IMREAD_UNCHANGED)
    if img is None:
        raise FileNotFoundError(path)
    return img.shape[1], img.shape[0]


def make_plan(job: dict, frames: list[str]) -> dict:
    import torch
    width, height = frame_size(frames[0])
    if sys.platform == "win32":
        return cuda_plan(job, width, height, len(frames), torch)
    brand = mac_brand()
    gpu_gb = torch.mps.recommended_max_memory() / 2**30
    budget = min(gpu_gb, available_system_gb()) * MEM_USE_FRACTION
    if job["window"] == "auto":
        window = choose_window(width, height, len(frames), budget)
    else:
        window = max(1, min(int(job["window"]), len(frames)))
    precision = choose_precision(brand) if job["precision"] == "auto" else job["precision"]
    pw, ph = padded_size(width, height)
    starts = chunk_starts(len(frames), window, max(1, window // 2))
    return {
        "mac": brand, "width": width, "height": height, "frames": len(frames),
        "device": "mps", "low_vram": False,
        "window": window, "stride": max(1, window // 2), "precision": precision,
        "chunks": len(starts),
        "gpu_gb": round(gpu_gb, 1), "budget_gb": round(budget, 1),
        "estimate_gb": round(estimate_peak_gb(pw, ph, window), 1),
    }


def cuda_plan(job: dict, width: int, height: int, count: int, torch) -> dict:
    if not torch.cuda.is_available():
        raise RuntimeError("The NVIDIA CUDA GPU is unavailable. Run Install Model to check the runtime.")
    sys.path.insert(0, job["model_dir"])
    import corridorkey.runtime as rt

    free, total = torch.cuda.mem_get_info()
    budget = free / 2**30 * MEM_USE_FRACTION
    padded_w, padded_h = padded_size(width, height)
    mpx = padded_w * padded_h / 1e6
    if job["window"] == "auto":
        window, _, low_vram, estimate = rt.select_vram_profile((padded_h, padded_w), count, budget)
        window = min(window, count)
    else:
        window = max(1, min(int(job["window"]), count))
        normal = rt.estimate_peak_vram_gb(mpx, window, False)
        low_vram = normal * 1.1 + 0.3 > budget
        estimate = rt.estimate_peak_vram_gb(mpx, window, low_vram)
    stride = max(1, window // 2)
    return {
        "gpu": torch.cuda.get_device_name(0), "device": "cuda", "low_vram": low_vram,
        "width": width, "height": height, "frames": count, "precision": "bf16",
        "window": window, "stride": stride, "chunks": len(chunk_starts(count, window, stride)),
        "gpu_gb": round(total / 2**30, 1), "budget_gb": round(budget, 1),
        "estimate_gb": round(estimate * 1.1 + 0.3, 1),
    }


# --- the run (talks to CorridorKey) ----------------------------------------

def run(job: dict, plan: dict, frames: list[str]) -> None:
    if plan["device"] == "mps":
        import ck2_mps
        ck2_mps.apply(plan["precision"])

    model_dir = job["model_dir"]
    sys.path.insert(0, model_dir)
    import cv2  # noqa: F401  (after OPENCV_IO_ENABLE_OPENEXR, set by corridorkey)
    import corridorkey.video_io as vio
    import corridorkey.runtime as rt
    import numpy as np

    out_dir = Path(job["output_dir"])
    out_dir.mkdir(parents=True, exist_ok=True)
    work = Path(job.get("work_dir") or (Path(job["frames_dir"]).parent / "ck2_work"))
    shutil.rmtree(work, ignore_errors=True)
    work.mkdir(parents=True)

    names = job["output_names"]
    total = len(frames)
    starts = chunk_starts(total, plan["window"], plan["stride"])
    if len(names) != total:
        raise ValueError(f"{len(names)} output names for {total} frames")

    # CorridorKey hands each finished frame to its EXR writers, one for the
    # alpha and one for the foreground. Catch both and write one RGBA cache
    # file per frame straight away: half float, DWAB level 45 (lossy for RGB,
    # lossless for the matte), like every EXR CorridorKey 2 for Resolve makes.
    from concurrent.futures import ThreadPoolExecutor
    # Written with the OpenEXR package: OpenCV's DWAB files have no channels.
    import OpenEXR
    header = {"compression": OpenEXR.DWAB_COMPRESSION, "type": OpenEXR.scanlineimage,
              "dwaCompressionLevel": 45.0}
    pending: dict[int, dict] = {}
    pool = ThreadPoolExecutor(max_workers=3)
    futures = []
    done = {"n": 0}

    def save(i: int, fg_bgr, alpha) -> None:
        fg = np.clip(fg_bgr[..., :3], -65504, 65504).astype(np.float16)
        channels = {"R": np.ascontiguousarray(fg[..., 2]), "G": np.ascontiguousarray(fg[..., 1]),
                    "B": np.ascontiguousarray(fg[..., 0]),
                    "A": np.ascontiguousarray(alpha.astype(np.float16))}
        tmp = out_dir / f".{names[i]}.part.exr"
        with OpenEXR.File(dict(header), channels) as f:
            f.write(str(tmp))
        os.replace(tmp, out_dir / names[i])

    def write_many(self, arr):
        if self.frame_name not in ("alpha", "fg"):
            return
        idx0 = self.frame_idx
        self.frame_idx += int(arr.shape[0])
        for j in range(arr.shape[0]):
            i = idx0 + j
            slot = pending.setdefault(i, {})
            slot[self.frame_name] = arr[j] if arr[j].ndim == 3 else arr[j][..., None]
            if len(slot) == 2:
                del pending[i]
                futures.append(pool.submit(save, i, slot["fg"], slot["alpha"][..., 0]))
                done["n"] += 1
                emit("progress", stage="keying", done=done["n"], total=total,
                     chunk=chunk_of_frame(done["n"], starts, plan["window"]), chunks=len(starts))

    vio.ExrSequenceWriter.write_many = write_many

    emit("status", message="loading model")
    t0 = time.time()
    paths = rt.RuntimePaths.from_package(model_dir)
    model = rt.load_model(paths, device=plan["device"])
    emit("status", message="model loaded", seconds=round(time.time() - t0, 1))

    hint = job["hint"]
    first_hint = every_hint = None
    if hint["mode"] == "first":
        first_hint = hint["dir"]
    elif hint["mode"] == "every":
        every_hint = hint["dir"]

    cfg = rt.InferenceConfig(
        hann_chunk=plan["window"], hann_stride=plan["stride"], auto_vram=False,
        low_vram=plan["low_vram"],
        carry_hint=bool(hint["carry"]), hint_quality=float(hint["strength"]),
        carry_hint_quality=float(hint["strength"]),
        outputs={"alpha", "fg"}, temp_dir=work / "tmp",
        frame_dir_linear=bool(job["input_linear"]),
        output_format="exr", exr_dtype="half")
    writer_cfg = vio.WriterConfig()
    try:
        rt.run_inference(model, job["frames_dir"], str(work / "out"), cfg, writer_cfg,
                         initial_hint_path=first_hint, hint_video_path=every_hint)
    finally:
        pool.shutdown(wait=True)
    del model
    for f in futures:
        f.result()                      # re-raise any write error
    if done["n"] != total or pending:
        raise RuntimeError(f"CorridorKey finished {done['n']} of {total} frames")
    shutil.rmtree(work, ignore_errors=True)
    if plan["device"] == "mps":
        emit("cpu_fallbacks", **ck2_mps.CPU_FALLBACKS)


def check(model_dir: str) -> None:
    """Loads the pinned model once and reports the GPU and an HD profile."""
    import torch
    if sys.platform == "win32":
        plan = cuda_plan({"model_dir": model_dir, "window": "auto"}, 1920, 1080, 80, torch)
        import corridorkey.runtime as rt
        t0 = time.time()
        model = rt.load_model(rt.RuntimePaths.from_package(model_dir), device="cuda")
        del model
        emit("check", ok=True, hd_window=plan["window"], load_seconds=round(time.time() - t0, 1), **plan)
        return
    import ck2_mps
    brand = mac_brand()
    precision = choose_precision(brand)
    ck2_mps.apply(precision)
    sys.path.insert(0, model_dir)
    import corridorkey.runtime as rt
    t0 = time.time()
    model = rt.load_model(rt.RuntimePaths.from_package(model_dir), device="mps")
    gpu_gb = torch.mps.recommended_max_memory() / 2**30
    del model
    emit("check", ok=True, mac=brand, precision=precision, gpu_gb=round(gpu_gb, 1),
         hd_window=choose_window(1920, 1080, 10**6, gpu_gb * MEM_USE_FRACTION),
         load_seconds=round(time.time() - t0, 1))


def main(argv: list[str]) -> int:
    if len(argv) == 3 and argv[1] == "check":
        real_stdout = sys.stdout
        sys.stdout = sys.stderr
        try:
            check(os.path.expanduser(argv[2]))
            return 0
        except Exception as exc:
            traceback.print_exc()
            emit("error", message=f"{type(exc).__name__}: {exc}")
            return 1
        finally:
            sys.stdout = real_stdout
    if len(argv) != 3 or argv[1] not in ("run", "plan"):
        print(__doc__ or "usage: ck2_worker.py run|plan <job.json>", file=sys.stderr)
        return 2
    os.umask(0o077)                  # Resolve starts processes with umask 000
    signal.signal(signal.SIGTERM, _on_sigterm)
    t0 = time.time()
    real_stdout = sys.stdout
    sys.stdout = sys.stderr          # CorridorKey's prints stay off the event stream
    try:
        job = load_job(argv[2])
        if not job["private"]:
            os.umask(0o022)                 # a shared Cache Folder: readable by others
        frames = list_frames(job["frames_dir"])
        if not frames:
            raise ValueError(f"no frames in {job['frames_dir']}")
        plan = make_plan(job, frames)
        if argv[1] == "plan":
            emit("plan", version=VERSION, **plan)
            return 0
        open_window(job.get("title") or "CorridorKey 2")
        emit("plan", version=VERSION, **plan)
        if plan["estimate_gb"] > plan["budget_gb"] and not job.get("skip_memory_check"):
            raise MemoryError(
                f"not enough memory: window {plan['window']} needs about "
                f"{plan['estimate_gb']} GB, {plan['budget_gb']} GB available")
        run(job, plan, frames)
        emit("done", frames=len(frames), seconds=round(time.time() - t0, 1),
             window=plan["window"], chunks=plan["chunks"])
        return 0
    except Cancelled:
        emit("error", message="Cancelled")
        return 1
    except Exception as exc:  # report every failure to the plugin
        traceback.print_exc()
        emit("error", message=f"{type(exc).__name__}: {exc}")
        return 1
    finally:
        close_window()
        sys.stdout = real_stdout


if __name__ == "__main__":
    sys.exit(main(sys.argv))
