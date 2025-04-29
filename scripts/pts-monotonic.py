import re
import pandas as pd
from datetime import timedelta
import matplotlib.pyplot as plt


def extract_pts_data(log_path):
    with open(log_path, "r", encoding="utf-8") as file:
        log_content = file.read()

    # extract pts and delta vs monotonic
    pattern = r"📍 PTS: (\d+\.\d+) ms \| ⏱️ Δ vs monotonic: (\d+\.\d+) ms"
    matches = re.findall(pattern, log_content)

    if not matches:
        raise ValueError("no pts data found. did you upload a grocery list instead?")

    df = pd.DataFrame(matches, columns=["PTS", "Delta_vs_Monotonic"])
    df["PTS"] = df["PTS"].astype(float)
    df["Delta_vs_Monotonic"] = df["Delta_vs_Monotonic"].astype(float)

    # calculate delta PTS between frames
    df["Delta_PTS"] = df["PTS"].diff()

    # calculate monotonic time (true timeline, PTS - drift)
    df["Monotonic"] = df["PTS"] - df["Delta_vs_Monotonic"]

    # normalize to start time
    start_time = df["Monotonic"].iloc[0]
    df["Time"] = (df["Monotonic"] - start_time).apply(
        lambda x: timedelta(milliseconds=x)
    )

    return df


def print_stats(df):
    print("\n📊 BASIC STATS:")
    columns = ["PTS", "Delta_PTS", "Monotonic", "Delta_vs_Monotonic"]
    for col in columns:
        print(f"\n{col}:")
        print(f"  Mean   : {df[col].mean():.3f}")
        print(f"  StdDev : {df[col].std():.3f}")
        print(f"  Min    : {df[col].min():.3f}")
        print(f"  Max    : {df[col].max():.3f}")
        print(f"  Median : {df[col].median():.3f}")


def plot_pts(df, camera_switch_times=None):
    plt.figure(figsize=(12, 6))

    # convert timedelta to seconds float for plotting
    time_sec = df["Time"].apply(lambda td: td.total_seconds())

    # force numpy arrays to shut pandas up
    plt.plot(time_sec.to_numpy(), df["PTS"].to_numpy(), label="PTS", marker="o")
    plt.plot(
        time_sec.to_numpy(),
        df["Monotonic"].to_numpy(),
        label="Monotonic",
        linestyle="--",
    )
    plt.plot(
        time_sec.to_numpy(), df["Delta_PTS"].to_numpy(), label="Δ PTS", linestyle=":"
    )

    if camera_switch_times:
        for t in camera_switch_times:
            plt.axvline(
                x=float(t.total_seconds()),
                color="red",
                linestyle="-",
                linewidth=1.5,
                label="Camera Switch" if t == camera_switch_times[0] else "",
            )

    plt.title("PTS vs Monotonic Time")
    plt.xlabel("Time since start (s)")
    plt.ylabel("Time (ms)")
    plt.legend()
    plt.grid(True)
    plt.tight_layout()
    plt.show()


if __name__ == "__main__":
    log_file = "video-hub.2025-04-24_17_03_08.log"
    df = extract_pts_data(log_file)

    print_stats(df)

    # TODO: provide actual camera switch event times if you ever decide to be useful
    camera_switches = [
        # timedelta(seconds=2),
    ]

    plot_pts(df, camera_switch_times=camera_switches)
