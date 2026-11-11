"""SI waveforms from Altium CSV exports -> vector PDF figures for the SI report.

Usage (from the Documentation folder):  python SI_Data/plot_si.py SI_Data SI_Figures
Input: CSV files exported from the Altium SimData document (click in a plot, File > Export > Plot),
       renamed as spi1_<net>[_noR|_33R]_<pin>[_driver].csv (sweep: spi1_sck_sweep_10-56R_B4-26.csv).
Output: si_wave_miso.pdf, si_wave_mosi.pdf, si_wave_sck.pdf, si_wave_sck_sweep.pdf (in SI_Figures),
        si_metrics.json (in SI_Data, next to the CSV files).
Requires: numpy, pandas, matplotlib.
"""
import sys, json
import numpy as np, pandas as pd
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

SRC, OUT = sys.argv[1], sys.argv[2]
plt.rcParams.update({
    "font.family": "serif", "font.serif": ["cmr10"], "mathtext.fontset": "cm",
    "axes.formatter.use_mathtext": True, "axes.unicode_minus": False,
    "font.size": 9, "axes.titlesize": 9, "axes.labelsize": 9, "legend.fontsize": 8,
    "xtick.labelsize": 8, "ytick.labelsize": 8,
    "axes.edgecolor": "#8a8986", "axes.linewidth": 0.6, "xtick.color": "#52514e", "ytick.color": "#52514e",
    "axes.labelcolor": "#0b0b0b", "text.color": "#0b0b0b",
    "axes.grid": True, "grid.color": "#e4e3df", "grid.linewidth": 0.5,
    "axes.spines.top": False, "axes.spines.right": False, "lines.linewidth": 1.2,
    "pdf.fonttype": 42,
})
BLUE, ORANGE, INK2, BAND = "#2a78d6", "#eb6834", "#52514e", "#d9d8d3"
W = 6.26
STM = (0.99, 2.31, "STM32 thresholds")
TXB = (1.05, 2.34, "TXB0108 thresholds")

def load(name):
    d = pd.read_csv(f"{SRC}/{name}")
    return d.iloc[:, 0].values * 1e9, d

def col(name, k=1):
    t, d = load(name); return t, d.iloc[:, k].values

def metrics(t, v, vih):
    # ringing minimum: lowest point after the first overshoot peak (a shoulder on the edge itself is not a re-entry)
    i0 = np.where((t > 10) & (v > vih))[0][0]
    w = (t >= t[i0]) & (t <= t[i0] + 4)
    tp = t[w][v[w].argmax()]
    seg = (t > tp) & (t < 60)
    j0 = np.where((t > 60) & (v < 0.99))[0][0]
    w2 = (t >= t[j0]) & (t <= t[j0] + 4)
    tu = t[w2][v[w2].argmin()]
    return dict(peak=v[(t > 10) & (t < 60)].max(), min_rise=v[seg].min(), t_min=t[seg][v[seg].argmin()],
                max_fall=v[t > tu].max(), under=v[t > 60].min())

def band(ax, th):
    ax.axhspan(th[0], th[1], color=BAND, alpha=0.55, lw=0, zorder=0)
    for y in th[:2]:
        ax.axhline(y, color=INK2, lw=0.6, ls=(0, (3, 2)), zorder=1)

def mark_min(ax, t, v, th, color, up=False):
    m = metrics(t, v, th[1])
    ax.plot([m["t_min"]], [m["min_rise"]], "o", ms=4, color=color, mec="white", mew=0.8, zorder=5)
    ax.annotate(f"{m['min_rise']:.2f} V", (m["t_min"], m["min_rise"]), xytext=(5, 7) if up else (5, -12),
                textcoords="offset points", fontsize=8, color="#0b0b0b", zorder=6,
                bbox=dict(boxstyle="square,pad=0.1", fc="white", ec="none", alpha=0.8))
    return m

def stack(rows, fname, legend=None):
    fig, axs = plt.subplots(len(rows), 1, figsize=(W, 1.75 * len(rows)), sharex=True)
    res = {}
    for ax, (title, series, th) in zip(axs, rows):
        if th: band(ax, th)
        for lab, t, v, c, mk in series:
            ax.plot(t, v, color=c, label=lab, zorder=3)
            if th and mk:
                res[f"{title} | {lab}"] = mark_min(ax, t, v, th, c, up=(c == BLUE and len(series) > 1))
        ax.set_title(title, loc="left", fontsize=9)
        ax.set_ylabel("V")
        ax.set_ylim(-1.2, 5.8); ax.set_xlim(0, 100)
        if th:
            ax.text(99, th[1] + 0.1, th[2], ha="right", va="bottom", fontsize=7.5, color=INK2)
    axs[-1].set_xlabel("Time (ns)")
    if legend:
        axs[0].legend(loc="upper right", frameon=False, ncol=2)
    fig.tight_layout(h_pad=0.6)
    fig.savefig(f"{OUT}/{fname}"); plt.close(fig)
    return res

R = {}
# MISO (B4 drives through R4 = 33 ohm)
R.update(stack([
    ("MCU B4, pin 27 (driver, through R4)", [("", *col("spi1_miso_B4-27_driver.csv"), BLUE, False)], None),
    ("Jetson, P2 pin 22 (receiver)", [("", *col("spi1_miso_P2-22.csv"), BLUE, True)], TXB),
    ("MCU B3, pin 27 (high impedance)", [("", *col("spi1_miso_B3-27.csv"), BLUE, True)], STM),
], "si_wave_miso.pdf"))
# MOSI
R.update(stack([
    ("Jetson, P2 pin 37 (driver)", [("", *col("spi1_mosi_P2-37_driver.csv"), BLUE, False)], None),
    ("MCU B3, pin 28", [("", *col("spi1_mosi_B3-28.csv"), BLUE, True)], STM),
    ("MCU B4, pin 28", [("", *col("spi1_mosi_B4-28.csv"), BLUE, True)], STM),
], "si_wave_mosi.pdf"))
# SCK before / after
R.update(stack([
    ("Jetson, P2 pin 13 (driver)", [("no series resistor", *col("spi1_sck_noR_P2-13_driver.csv"), ORANGE, False),
                                     (r"33 $\Omega$ on the layout", *col("spi1_sck_33R_P2-13_driver.csv"), BLUE, False)], None),
    ("MCU B3, pin 26", [("no series resistor", *col("spi1_sck_noR_B3-26.csv"), ORANGE, True),
                        (r"33 $\Omega$ on the layout", *col("spi1_sck_33R_B3-26.csv"), BLUE, True)], STM),
    ("MCU B4, pin 26", [("no series resistor", *col("spi1_sck_noR_B4-26.csv"), ORANGE, True),
                        (r"33 $\Omega$ on the layout", *col("spi1_sck_33R_B4-26.csv"), BLUE, True)], STM),
], "si_wave_sck.pdf", legend=True))

# Sweep
t, d = load("spi1_sck_sweep_10-56R_B4-26.csv")
vals = np.linspace(10, 56, 10)          # Serial Res sweep 10-56 ohm, 10 steps (linear)
ramp = ["#86b6ef", "#6da7ec", "#5598e7", "#3987e5", "#2a78d6", "#256abf", "#1c5cab", "#184f95", "#104281", "#0d366b"]
tn, vn = col("spi1_sck_noR_B4-26.csv")
tr, vr = col("spi1_sck_33R_B4-26.csv")
fig, (a1, a2) = plt.subplots(1, 2, figsize=(W, 2.6), gridspec_kw=dict(width_ratios=[1.6, 1]))
band(a1, STM)
a1.plot(tn, vn, color=ORANGE, label="no series resistor", zorder=4)
for k, (rv, c) in enumerate(zip(vals, ramp)):
    a1.plot(t, d.iloc[:, 2 + k].values, color=c, lw=0.9, zorder=3, label=(r"sweep 10 $\Omega$ (light) $\to$ 56 $\Omega$ (dark)") if k == 9 else None)
a1.set_xlim(8, 40); a1.set_ylim(-1.2, 5.8)
a1.set_xlabel("Time (ns)"); a1.set_ylabel("V"); a1.set_title("MCU B4, pin 26 (rising edge)", loc="left")
a1.legend(loc="upper right", frameon=False)
mins = [metrics(t, d.iloc[:, 2 + k].values, STM[1])["min_rise"] for k in range(10)]
m0 = metrics(tn, vn, STM[1])["min_rise"]; mr = metrics(tr, vr, STM[1])["min_rise"]
a2.axhline(STM[1], color=INK2, lw=0.6, ls=(0, (3, 2)))
a2.text(6, STM[1] - 0.04, r"$V_{IH}$ = 2.31 V", ha="left", va="top", fontsize=7.5, color=INK2)
a2.plot(vals, mins, "-o", color=BLUE, ms=4, mec="white", mew=0.8, label="sweep (resistor at driver)")
a2.plot([33], [mr], "D", color="#0b0b0b", ms=4.5, label=r"33 $\Omega$ on the layout")
a2.annotate(f"{mr:.2f} V", (33, mr), xytext=(6, -12), textcoords="offset points", fontsize=8)
a2.set_xlabel(r"Series resistor ($\Omega$)"); a2.set_ylabel("Minimum after rise (V)")
a2.set_xlim(5, 60); a2.set_ylim(2.0, 3.4); a2.set_title(f"without resistor: {m0:.2f} V", loc="left")
a2.legend(loc="upper left", frameon=False)
fig.tight_layout(w_pad=1.2)
fig.savefig(f"{OUT}/si_wave_sck_sweep.pdf"); plt.close(fig)
R["sweep"] = {f"{v:.1f}": round(m, 3) for v, m in zip(vals, mins)}
R["sweep_peaks"] = {f"{vals[k]:.1f}": round(metrics(t, d.iloc[:, 2 + k].values, STM[1])["peak"], 3) for k in range(10)}
json.dump({k: ({kk: round(float(vv), 3) for kk, vv in v.items()} if isinstance(v, dict) else v) for k, v in R.items()},
          open(f"{SRC}/si_metrics.json", "w"), indent=1)
print(json.dumps(R, indent=1, default=float))
