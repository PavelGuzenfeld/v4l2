import re
import pandas as pd
from datetime import timedelta
import matplotlib.pyplot as plt


def extract_pts_data(log_path):
    with open(log_path, "r", encoding="utf-8") as file:
        log_content = file.read()

    pattern = r"📍 PTS: (\d+\.\d+) ms \| ⏱️ Δ vs monotonic: (\d+\.\d+) ms"
    matches = re.findall(pattern, log_content)

    if not matches:
        raise ValueError("no pts data found. did you upload a grocery list instead?")

    df = pd.DataFrame(matches, columns=["PTS", "Latency"])
    df["PTS"] = df["PTS"].astype(float)
    df["Latency"] = df["Latency"].astype(float)
    df["Delta_PTS"] = df["PTS"].diff()
    df["System_TS"] = df["PTS"] - df["Latency"]

    start_time = df["PTS"].iloc[0]
    df["Time"] = df["PTS"] - start_time

    return df


def print_stats(df):
    print("\n📊 BASIC STATS:")
    columns = ["PTS", "Delta_PTS", "System_TS", "Latency"]
    for col in columns:
        print(f"\n{col}:")
        print(f"  Mean   : {df[col].mean():.3f}")
        print(f"  StdDev : {df[col].std():.3f}")
        print(f"  Min    : {df[col].min():.3f}")
        print(f"  Max    : {df[col].max():.3f}")
        print(f"  Median : {df[col].median():.3f}")

    # frame skip stats
    skips = df["Frame_Skips"]
    total_skips = skips.sum()
    skipped_points = skips[skips > 0]

    print("\n📉 Frame Skips (Inferred):")
    print(f"  Total Skips      : {int(total_skips)}")
    print(
        f"  Affected Samples : {len(skipped_points)} / {len(df)} ({100 * len(skipped_points) / len(df):.2f}%)"
    )
    print(
        f"  Max Skip         : {int(skipped_points.max()) if not skipped_points.empty else 0}"
    )
    print(
        f"  Mean Skip        : {skipped_points.mean():.2f}"
        if not skipped_points.empty
        else "  Mean Skip        : N/A"
    )
    print(
        f"  Median Skip      : {skipped_points.median():.2f}"
        if not skipped_points.empty
        else "  Median Skip      : N/A"
    )


def normalize_columns(df, columns):
    for col in columns:
        mean = df[col].mean()
        std = df[col].std()
        df[f"{col}_norm"] = (df[col] - mean) / std
    return df


def plot_histograms(df, columns):
    for col in columns:
        plt.figure(figsize=(8, 4))
        values = df[col].dropna()
        weights = [100.0 / len(values)] * len(values)
        plt.hist(values, bins=30, alpha=0.75, edgecolor="black", weights=weights)
        plt.title(f"Histogram of {col} (percentage)")
        plt.xlabel(col)
        plt.ylabel("Percentage (%)")
        plt.grid(True)
        plt.tight_layout()
        plt.show()


def plot_pts(df, camera_switch_times=None):
    plt.figure(figsize=(12, 6))

    # x-axis: ms diff from first PTS
    time_ms = df["PTS"] - df["PTS"].iloc[0]

    # scale to match Delta_PTS scale
    pts_scaled = df["PTS"] - df["PTS"].iloc[0]
    sys_scaled = df["System_TS"] - df["System_TS"].iloc[0]
    scale_factor = df["Delta_PTS"].std() / pts_scaled.std()
    pts_scaled *= scale_factor
    sys_scaled *= scale_factor

    plt.plot(
        time_ms.to_numpy(), pts_scaled.to_numpy(), label="PTS (scaled)", marker="o"
    )
    plt.plot(
        time_ms.to_numpy(),
        sys_scaled.to_numpy(),
        label="System TS (scaled)",
        linestyle="--",
    )
    plt.plot(
        time_ms.to_numpy(), df["Delta_PTS"].to_numpy(), label="Δ PTS", linestyle=":"
    )

    if camera_switch_times:
        for t in camera_switch_times:
            x_ms = (df["PTS"].iloc[0] + t.total_seconds() * 1000) - df["PTS"].iloc[0]
            plt.axvline(
                x=x_ms,
                color="red",
                linestyle="-",
                linewidth=1.5,
                label="Camera Switch" if t == camera_switch_times[0] else "",
            )

    plt.title("Scaled PTS vs System_TS vs ΔPTS (X: ms from first PTS)")
    plt.xlabel("Time since start (ms)")
    plt.ylabel("Time (scaled to ΔPTS std)")
    plt.legend()
    plt.grid(True)
    plt.tight_layout()
    plt.show()


import numpy as np  # make sure this is at the top


def plot_pts_and_latency(df, camera_switch_times=None):
    plt.figure(figsize=(12, 6))

    # x-axis: ms diff from first PTS
    time_ms = df["PTS"] - df["PTS"].iloc[0]

    # scale latency too
    latency_scaled = df["Latency"] * (df["Delta_PTS"].std() / df["Latency"].std())

    # plot main signals
    plt.plot(
        time_ms.to_numpy(), df["Delta_PTS"].to_numpy(), label="Δ PTS", linestyle=":"
    )
    plt.plot(
        time_ms.to_numpy(),
        latency_scaled.to_numpy(),
        label="Latency (scaled)",
        linestyle="-.",
        color="tab:red",
    )

    # # overlay lost frames
    # lost_points = df[
    #     (df["Frame_Skips"] > 0) & df["Delta_PTS"].notna() & time_ms.notna()
    # ]
    # delta_max = df["Delta_PTS"].dropna().max() * 1.1

    # for i, row in lost_points.iterrows():
    #     ts = time_ms.iloc[i]
    #     if not pd.isna(ts) and np.isfinite(ts):
    #         plt.axvline(x=ts, color="black", linestyle="--", alpha=0.4)
    #         plt.text(
    #             ts,
    #             delta_max,
    #             f"-{row['Frame_Skips']}",
    #             rotation=90,
    #             ha="center",
    #             va="bottom",
    #             fontsize=5,
    #             color="black",
    #         )

    # camera switches
    if camera_switch_times:
        for t in camera_switch_times:
            x_ms = (df["PTS"].iloc[0] + t.total_seconds() * 1000) - df["PTS"].iloc[0]
            plt.axvline(
                x=x_ms,
                color="blue",
                linestyle="--",
                linewidth=1.5,
                label="Camera Switch" if t == camera_switch_times[0] else "",
            )

    plt.title("PTS, ΔPTS, Latency (scaled) + Inferred Frame Loss")
    plt.xlabel("Time since start (ms)")
    plt.ylabel("Scaled Units")
    plt.legend()
    plt.grid(True)
    plt.tight_layout()
    plt.show()


def estimate_dropped_frames(df, tolerance=1.5):
    """
    Estimate dropped frames based on excessive delta_PTS values.

    Parameters:
    - tolerance: float, multiplier on the median delta_PTS to allow before flagging
    """
    expected_interval = df["Delta_PTS"].median()
    df["Expected_Delta"] = expected_interval
    df["Frame_Skips"] = (
        (df["Delta_PTS"] / expected_interval).round().fillna(1) - 1
    ).astype(int)
    df["Frame_Skips"] = df["Frame_Skips"].apply(lambda x: x if x > 0 else 0)
    return df


if __name__ == "__main__":
    log_file = "video-hub.2025-04-24_17_03_08.log"
    df = extract_pts_data(log_file)

    columns = ["PTS", "Delta_PTS", "System_TS", "Latency"]
    df = normalize_columns(df, columns)
    df = estimate_dropped_frames(df)
    print_stats(df)
    # plot_histograms(df, columns + [f"{col}_norm" for col in columns])

    camera_switches = [
        # timedelta(seconds=2),  # add real values here
    ]

    # plot_pts(df, camera_switch_times=camera_switches)
    # plot_latency_time_series(df, camera_switch_times=camera_switches)
    plot_pts_and_latency(df, camera_switch_times=camera_switches)
