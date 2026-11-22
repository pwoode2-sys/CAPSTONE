"""Capture a MAX30102/ESP32 session and automatically plot its results.

Works with MAX30102_PLOT_WRITE.ino 
Produces, on every successful capture:
  - capture.txt: complete serial log
  - samples.csv: timestamped IR/RED samples
  - vitals.csv: algorithm results and validity flags
  - heart_rate_histogram.png
  - spo2_histogram.png
  - optical_amplitude_vs_time.png

Required packages (install into the SAME environment used to run this file):
  python -m pip install pyserial matplotlib

Close the Arduino Serial Monitor/Plotter before running.
"""

import csv
import math
import time
from datetime import datetime
from pathlib import Path

import matplotlib.pyplot as plt
import numpy as np

# -------------------------- SETTINGS --------------------------
PORT = "COM3"
BAUD_RATE = 115200
STARTUP_GRACE_SECONDS = 3
START_RETRY_SECONDS = 3
WAIT_FOR_START_SECONDS = 30
MAX_CAPTURE_SECONDS = 25
SHOW_PLOTS = True  # Set False to save PNGs without opening plot windows

# Each run gets its own folder, so earlier measurements are preserved.
OUTPUT_ROOT = Path(__file__).resolve().parent / "MAX30102_runs"


def parse_log_lines(lines):
    """Extract raw samples and rolling-window vitals from the ESP32 text log.

    The Arduino sends:
      SAMPLE,elapsed_ms,IR,RED
      WINDOW_END_MS,elapsed_ms
      Raw Heart Rate: 75
      Heart Rate Valid Flag: 1
      Raw SpO2: 98
      SpO2 Valid Flag: 1
    """
    samples = []
    windows = []
    window = None

    def save_window():
        if window is not None and window.get("time_s") is not None:
            # Only store results if the algorithm actually printed its flags.
            if "hr_valid" in window and "spo2_valid" in window:
                windows.append(window.copy())

    for raw in lines:
        line = raw.strip()
        if line.startswith("SAMPLE,"):
            fields = line.split(",")
            if len(fields) == 4:
                try:
                    ms, ir, red = (int(s) for s in fields[1:])
                    if ms >= 0:
                        samples.append({"time_s": ms / 1000.0, "IR": ir, "RED": red})
                except ValueError:
                    pass
            continue

        if line.startswith("WINDOW_END_MS,"):
            save_window()
            try:
                ms = int(line.split(",", 1)[1])
                window = {"time_s": ms / 1000.0, "heart_rate": None,
                          "hr_valid": None, "spo2": None, "spo2_valid": None}
                # Remove unpopulated flag keys so incomplete reports aren't used.
                del window["hr_valid"]
                del window["spo2_valid"]
            except ValueError:
                window = None
            continue

        if window is None:
            continue
        try:
            if line.startswith("Raw Heart Rate:"):
                window["heart_rate"] = int(line.split(":", 1)[1].strip())
            elif line.startswith("Heart Rate Valid Flag:"):
                window["hr_valid"] = int(line.split(":", 1)[1].strip())
            elif line.startswith("Raw SpO2:"):
                window["spo2"] = int(line.split(":", 1)[1].strip())
            elif line.startswith("SpO2 Valid Flag:"):
                window["spo2_valid"] = int(line.split(":", 1)[1].strip())
        except ValueError:
            pass

    save_window()
    return samples, windows


def capture_serial(log_path):
    """Request one recording, store all received lines, and return capture status."""
    import serial  # pyserial; inside function so plotting can be tested offline

    all_lines = []
    started = False
    ended = False
    sample_count = 0

    print(f"Opening {PORT} at {BAUD_RATE} baud...")
    with serial.Serial(PORT, BAUD_RATE, timeout=0.2) as esp, log_path.open(
            "w", encoding="utf-8", newline="\n"
    ) as out:
        port_opened = time.monotonic()
        last_start_request = float("-inf")
        began_at = None

        while True:
            now = time.monotonic()
            if not started and now - port_opened > WAIT_FOR_START_SECONDS:
                break
            if started and now - began_at > MAX_CAPTURE_SECONDS:
                break

            if (not started
                    and now - port_opened >= STARTUP_GRACE_SECONDS
                    and now - last_start_request >= START_RETRY_SECONDS):
                esp.write(b"S")
                esp.flush()
                last_start_request = time.monotonic()
                print("[PC] Sent S to request 10-second capture")

            incoming = esp.readline()
            if not incoming:
                continue

            line = incoming.decode("utf-8", errors="replace").rstrip("\r\n")
            if not line:
                continue
            out.write(line + "\n")
            out.flush()
            all_lines.append(line)

            if line == "LOG_BEGIN":
                started = True
                began_at = time.monotonic()
                print("[ESP32] Capture started")
            elif line == "LOG_END":
                ended = True
                print("[ESP32] Capture ended")
                break
            elif line.startswith("SAMPLE,") and started:
                sample_count += 1
                if sample_count % 50 == 0:
                    print(f"  {sample_count} raw samples received")
            elif not started:
                print(f"[ESP32] {line}")
                # ESP32 may have finished booting before the grace delay.
                if "Send S over serial" in line and now - last_start_request > 0.5:
                    esp.write(b"S")
                    esp.flush()
                    last_start_request = time.monotonic()
                    print("[PC] ESP32 ready; sent S")
            elif line.startswith("ERROR:"):
                print(f"[ESP32] {line}")

            if "MAX30102 not found" in line:
                print("Sensor not detected; inspect wiring and power.")
                break

    return all_lines, started, ended, sample_count


def save_csvs(samples, windows, folder):
    if samples:
        with (folder / "samples.csv").open("w", newline="", encoding="utf-8") as f:
            writer = csv.DictWriter(f, fieldnames=["time_s", "IR", "RED"])
            writer.writeheader()
            writer.writerows(samples)
    if windows:
        with (folder / "vitals.csv").open("w", newline="", encoding="utf-8") as f:
            writer = csv.DictWriter(f, fieldnames=["time_s", "heart_rate", "hr_valid",
                                                   "spo2", "spo2_valid"])
            writer.writeheader()
            writer.writerows(windows)


def plot_results(samples, windows, folder):
    """Save three *separate* figure files, and optionally display all of them."""
    # Accepted by library flag only; acceptance does not prove medical accuracy.
    bpm = [w["heart_rate"] for w in windows
           if w.get("hr_valid") == 1 and w.get("heart_rate") is not None
           and 0 < w["heart_rate"] < 300]
    spo2 = [w["spo2"] for w in windows
            if w.get("spo2_valid") == 1 and w.get("spo2") is not None
            and 0 <= w["spo2"] <= 100]

    figures = []

    # 1) Heart-rate histogram: one count per accepted rolling window
    fig, ax = plt.subplots(figsize=(9, 5))
    if bpm:
        lo = math.floor(min(bpm) / 10) * 10
        hi = math.ceil(max(bpm) / 10) * 10 + 10
        edges = np.arange(lo, hi + 1, 10)
        ax.hist(bpm, bins=edges, edgecolor="black", alpha=0.8)
        ax.axvline(float(np.mean(bpm)), linestyle="--", linewidth=1.8,
                   label=f"Mean = {np.mean(bpm):.1f} BPM")
        ax.legend()
    else:
        ax.text(0.5, 0.5, "No algorithm-accepted heart-rate readings",
                transform=ax.transAxes, ha="center", va="center")
    ax.set(title="Heart Rate Distribution (10-second capture)",
           xlabel="Heart rate (BPM)", ylabel="Number of accepted windows")
    ax.grid(axis="y", alpha=0.25)
    fig.tight_layout()
    path = folder / "heart_rate_histogram.png"
    fig.savefig(path, dpi=180)
    figures.append(fig)
    print(f"Saved: {path}")

    # 2) SpO2 histogram: centered on integer percentage values
    fig, ax = plt.subplots(figsize=(9, 5))
    if spo2:
        edges = np.arange(min(spo2) - 0.5, max(spo2) + 1.5, 1)
        ax.hist(spo2, bins=edges, edgecolor="black", alpha=0.8)
        ax.axvline(float(np.mean(spo2)), linestyle="--", linewidth=1.8,
                   label=f"Mean = {np.mean(spo2):.1f}%")
        ax.legend()
        ax.set_xticks(np.arange(min(spo2), max(spo2) + 1, 1))
    else:
        ax.text(0.5, 0.5, "No algorithm-accepted SpO2 readings",
                transform=ax.transAxes, ha="center", va="center")
    ax.set(title="Blood Oxygen (SpO2) Distribution (10-second capture)",
           xlabel="SpO2 (%)", ylabel="Number of accepted windows")
    ax.grid(axis="y", alpha=0.25)
    fig.tight_layout()
    path = folder / "spo2_histogram.png"
    fig.savefig(path, dpi=180)
    figures.append(fig)
    print(f"Saved: {path}")

    # 3) Time-domain signal plot: RAW optical ADC values, not SpO2 amplitude
    fig, ax = plt.subplots(figsize=(10, 5))
    if samples:
        t = [s["time_s"] for s in samples]
        ax.plot(t, [s["IR"] for s in samples], label="Infrared (IR)", linewidth=1.3)
        ax.plot(t, [s["RED"] for s in samples], label="Red LED", linewidth=1.3)
        ax.legend()
    else:
        ax.text(0.5, 0.5, "No SAMPLE timestamp records received.\n"
                          "Upload MAX30102_10s_diagnostics_fixed.ino.",
                transform=ax.transAxes, ha="center", va="center")
    ax.set(title="MAX30102 Optical Signal Amplitude vs. Time",
           xlabel="Elapsed time (seconds)", ylabel="Raw ADC amplitude (counts)")
    ax.grid(alpha=0.25)
    fig.tight_layout()
    path = folder / "optical_amplitude_vs_time.png"
    fig.savefig(path, dpi=180)
    figures.append(fig)
    print(f"Saved: {path}")

    if bpm:
        print(f"Heart rate: {len(bpm)} accepted windows; mean = {np.mean(bpm):.1f} BPM")
    else:
        print("Heart rate: no accepted windows in this session.")
    if spo2:
        print(f"SpO2: {len(spo2)} accepted windows; mean = {np.mean(spo2):.1f}%")
    else:
        print("SpO2: no accepted windows in this session.")
    print("Note: adjacent windows share samples, and validity flags are not proof of accuracy.")

    if SHOW_PLOTS:
        print("Opening graphs; close the plot windows when finished.")
        plt.show()
    for fig in figures:
        plt.close(fig)


def main():
    run_name = datetime.now().strftime("%Y-%m-%d_%H-%M-%S")
    folder = OUTPUT_ROOT / run_name
    folder.mkdir(parents=True, exist_ok=True)
    log_path = folder / "capture.txt"
    print(f"Output folder: {folder}")

    lines, started, ended, sample_count = capture_serial(log_path)
    if not started:
        print("No LOG_BEGIN received; no charts were generated.")
        print("Check the startup messages, board sketch, and COM port.")
        print(f"Saved diagnostic startup log: {log_path}")
        return

    samples, windows = parse_log_lines(lines)
    save_csvs(samples, windows, folder)
    if not ended:
        print("WARNING: Capture ended early. Plots will use the available data only.")
    print(f"Captured {sample_count} raw sample lines and {len(windows)} full vital reports.")
    plot_results(samples, windows, folder)
    print(f"All files are in: {folder}")


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("\nStopped by user.")
    except Exception as exc:
        # Show an actionable message for the most common port error.
        if "could not open port" in str(exc).lower() or "access is denied" in str(exc).lower():
            print(f"Serial-port error: {exc}")
            print("Close Arduino Serial Monitor and Serial Plotter, then retry.")
        else:
            raise
