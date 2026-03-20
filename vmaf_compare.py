#!/usr/bin/env python3
"""
Sunshine vs Moonlight VMAF 比較

フレーム番号 CSV を使って Sunshine/Moonlight 間のフレーム対応を取り、VMAF/SSIM を計算する。

=== 基本的な使い方 ===

  # CSV なし: フレーム数が一致する前提でそのまま比較
  python3 vmaf_compare.py \
    --reference sunshine_capture.mkv \
    --distorted moonlight_recording.yuv \
    --width 1920 --height 1080 --fps 60

  # CSV あり: frame_nr でフレーム対応を取って比較
  python3 vmaf_compare.py \
    --reference sunshine_capture.mkv \
    --distorted moonlight_recording.yuv \
    --ref-csv sunshine_capture.frames.csv \
    --dist-csv moonlight_recording.frames.csv \
    --width 1920 --height 1080 --fps 60

  # .meta ファイルから解像度を自動取得
  python3 vmaf_compare.py \
    --reference sunshine_capture.mkv \
    --distorted moonlight_recording.yuv \
    --ref-csv sunshine_capture.frames.csv \
    --dist-csv moonlight_recording.frames.csv \
    --meta sunshine_capture.mkv.meta

  # raw ファイル同士（pixfmt 指定）
  python3 vmaf_compare.py \
    --reference sunshine_capture.yuv --ref-pixfmt nv12 \
    --distorted moonlight_recording.yuv \
    --ref-csv sunshine_capture.frames.csv \
    --dist-csv moonlight_recording.frames.csv \
    --width 1920 --height 1080 --fps 60

  # 最初の300フレームだけ比較して JSON に保存
  python3 vmaf_compare.py \
    --reference sunshine_capture.mkv \
    --distorted moonlight_recording.yuv \
    --ref-csv sunshine_capture.frames.csv \
    --dist-csv moonlight_recording.frames.csv \
    --width 1920 --height 1080 --fps 60 \
    --frames 300 --output results.json

  # カスタム VMAF モデルを指定
  python3 vmaf_compare.py \
    --reference sunshine_capture.mkv \
    --distorted moonlight_recording.yuv \
    --width 1920 --height 1080 --fps 60 \
    --model-path /path/to/vmaf_v0.6.1.json

"""

import argparse
import csv
import json
import os
import struct
import subprocess
import sys
import tempfile
from dataclasses import dataclass, field
from datetime import datetime
from pathlib import Path
from typing import Dict, List, Optional


# ============================================================
#  データ構造
# ============================================================

@dataclass
class FrameAlignment:
    """フレーム対応の結果を保持する。"""
    matched_ref_indices: List[int] = field(default_factory=list)
    matched_dist_indices: List[int] = field(default_factory=list)
    matched_frame_nrs: List[int] = field(default_factory=list)
    dropped_frame_nrs: List[int] = field(default_factory=list)
    ref_total: int = 0
    dist_total: int = 0


# ============================================================
#  CSV 読み込み
# ============================================================

def load_frame_csv(path: str) -> List[Dict]:
    """フレーム CSV を読み込んで辞書のリストとして返す。"""
    rows = []
    with open(path, 'r', newline='') as f:
        reader = csv.DictReader(f)
        for row in reader:
            parsed = {}
            for k, v in row.items():
                k = k.strip()
                v = v.strip()
                # 数値に変換できるものは変換
                try:
                    if '.' in v:
                        parsed[k] = float(v)
                    else:
                        parsed[k] = int(v)
                except ValueError:
                    parsed[k] = v
            rows.append(parsed)
    return rows


def align_frames(ref_csv_path: str, dist_csv_path: str) -> FrameAlignment:
    """
    Sunshine 側と Moonlight 側の CSV を frame_nr で突き合わせる。

    """
    ref_rows = load_frame_csv(ref_csv_path)
    dist_rows = load_frame_csv(dist_csv_path)

    result = FrameAlignment()
    result.ref_total = len(ref_rows)
    result.dist_total = len(dist_rows)

    # Moonlight 側: frame_nr → local_frame_idx のマップ
    dist_by_frame_nr: Dict[int, int] = {}
    for row in dist_rows:
        fnr = row.get('frame_nr')
        idx = row.get('local_frame_idx')
        if fnr is not None and idx is not None:
            dist_by_frame_nr[fnr] = idx

    # Sunshine 側の全 frame_nr を走査して対応を取る
    for row in ref_rows:
        fnr = row.get('frame_nr')
        ref_idx = row.get('local_frame_idx')

        if fnr is None or ref_idx is None:
            continue

        if fnr in dist_by_frame_nr:
            result.matched_ref_indices.append(ref_idx)
            result.matched_dist_indices.append(dist_by_frame_nr[fnr])
            result.matched_frame_nrs.append(fnr)
        else:
            result.dropped_frame_nrs.append(fnr)

    return result


# ============================================================
#  YUV フレーム抽出
# ============================================================

def get_frame_size_yuv420p(width: int, height: int) -> int:
    """YUV420P の 1 フレームのバイト数。"""
    return width * height * 3 // 2


def extract_raw_frames(input_path: str, indices: List[int],
                       output_path: str, width: int, height: int,
                       pixfmt: str = 'yuv420p') -> int:
    # pixfmt に応じた 1 フレームのサイズ計算
    # NV12, YUV420P: W*H*1.5,  P010: W*H*3
    fmt_lower = pixfmt.lower()
    if fmt_lower in ('nv12', 'yuv420p'):
        frame_size = width * height * 3 // 2
    elif fmt_lower == 'p010' or fmt_lower == 'p010le':
        frame_size = width * height * 3  # 2 bytes per sample
    else:
        # 汎用: YUV420P と仮定
        frame_size = width * height * 3 // 2

    file_size = os.path.getsize(input_path)
    total_frames = file_size // frame_size

    if not indices:
        return 0

    # インデックスの範囲チェック
    max_idx = max(indices)
    if max_idx >= total_frames:
        print(f"警告: インデックス {max_idx} がフレーム数 {total_frames} を超えています",
              file=sys.stderr)

    written = 0
    with open(input_path, 'rb') as fin, open(output_path, 'wb') as fout:
        for idx in indices:
            if idx >= total_frames:
                continue
            fin.seek(idx * frame_size)
            data = fin.read(frame_size)
            if len(data) == frame_size:
                fout.write(data)
                written += 1

    return written


def extract_container_frames(input_path: str, indices: List[int],
                             output_path: str, width: int, height: int) -> int:
    """
    コンテナ (mkv/mp4) から指定インデックスのフレームだけを抽出して
    raw YUV420P ファイルに書き出す。

    ffmpeg で全フレームを YUV420P にデコードし、必要なフレームだけを残す。
    """
    # まず全フレームをデコード
    tmp_all = tempfile.NamedTemporaryFile(suffix='.yuv', delete=False)
    tmp_all_path = tmp_all.name
    tmp_all.close()

    cmd = [
        'ffmpeg', '-y', '-hide_banner', '-loglevel', 'warning',
        '-i', input_path,
        '-f', 'rawvideo', '-pix_fmt', 'yuv420p',
        tmp_all_path
    ]
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode != 0:
        print(f"ffmpeg デコードエラー: {result.stderr[-500:]}", file=sys.stderr)
        os.unlink(tmp_all_path)
        return 0

    # 指定フレームだけ抽出
    written = extract_raw_frames(tmp_all_path, indices, output_path,
                                 width, height, 'yuv420p')
    os.unlink(tmp_all_path)
    return written


# ============================================================
#  メタデータ / ファイル判定
# ============================================================

def parse_meta_file(meta_path: str) -> Dict[str, str]:
    meta = {}
    with open(meta_path, 'r') as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith('#'):
                continue
            if '=' in line:
                key, value = line.split('=', 1)
                meta[key.strip()] = value.strip()
    return meta


def get_file_type(path: str) -> str:
    ext = Path(path).suffix.lower()
    if ext in ('.mkv', '.mp4', '.avi', '.mov'):
        return 'container'
    elif ext in ('.yuv',):
        return 'rawvideo'
    else:
        return 'unknown'


# ============================================================
#  ffmpeg 入力引数の構築
# ============================================================

def build_input_args(path: str, pixfmt: str,
                     width: int, height: int, fps: int) -> List[str]:
    file_type = get_file_type(path)

    if file_type == 'container':
        return ['-i', path]
    elif file_type == 'rawvideo':
        if not pixfmt:
            pixfmt = 'yuv420p' if path.endswith('.yuv') else ''
        if not pixfmt:
            print(f"エラー: {path} の --ref-pixfmt または --dist-pixfmt を指定してください",
                  file=sys.stderr)
            sys.exit(1)
        return [
            '-f', 'rawvideo',
            '-pix_fmt', pixfmt,
            '-s', f'{width}x{height}',
            '-r', str(fps),
            '-i', path
        ]
    else:
        print(f"エラー: 未対応のファイル形式: {path}", file=sys.stderr)
        sys.exit(1)


# ============================================================
#  VMAF 計算
# ============================================================

def run_vmaf(reference: str, distorted: str,
             width: int, height: int, fps: int,
             ref_pixfmt: str = '', dist_pixfmt: str = '',
             frames: int = 0, verbose: bool = False,
             model_path: str = '', model_version: str = 'v2') -> dict:
    """ffmpeg の libvmaf フィルタで VMAF を計算する。"""

    tmp = tempfile.NamedTemporaryFile(suffix='.json', delete=False)
    log_path = tmp.name
    tmp.close()

    ref_args = build_input_args(reference, ref_pixfmt, width, height, fps)
    dist_args = build_input_args(distorted, dist_pixfmt, width, height, fps)

    # VMAF は常に YUV420P で計算する
    filter_chain = [
        "[0:v]format=yuv420p[dist420]",
        "[1:v]format=yuv420p[ref420]"
    ]

    n_threads = os.cpu_count() or 4

    # モデル指定の3つのモード:
    #
    # 1. --model-path /path/to/model.json  (ファイルパス)
    #    → model=path=/path/to/model.json
    #
    # 2. --model-path vmaf_b_v0.6.3  (built-in バージョン名)
    #
    # 3. --model-path 未指定
    #    → ffmpeg デフォルト (vmaf_v0.6.1)

    if model_path:
        if model_version == 'v1':
            model_str = f"model_path={model_path}:"
        elif '/' in model_path or model_path.endswith('.json'):
            # ファイルパスとして扱う
            escaped = model_path.replace('\\', '\\\\').replace(':', '\\:').replace('=', '\\=')
            model_str = f"model=path={escaped}:"
        else:
            # built-in バージョン名として扱う (例: vmaf_b_v0.6.3)
            model_str = f"model=version={model_path}:"
    else:
        model_str = ""

    vmaf_filter = (
        f"[dist420][ref420]libvmaf="
        f"{model_str}"
        f"log_path={log_path}:log_fmt=json:"
        f"n_threads={n_threads}:"
        f"feature=name=float_ssim"
    )
    vmaf_filter = ';'.join(filter_chain + [vmaf_filter])

    limit_args = ['-frames:v', str(frames)] if frames > 0 else []

    # libvmaf の入力順: distorted が [0:v], reference が [1:v]
    cmd = ['ffmpeg', '-y', '-hide_banner']
    cmd += dist_args
    cmd += ref_args
    cmd += limit_args
    cmd += ['-lavfi', vmaf_filter, '-f', 'null', '-']

    if verbose:
        print(f"コマンド: {' '.join(cmd)}")

    result = subprocess.run(cmd, capture_output=True, text=True, timeout=3600)

    if result.returncode != 0:
        print(f"ffmpeg エラー:", file=sys.stderr)
        print(result.stderr[-2000:], file=sys.stderr)
        if 'No such filter' in result.stderr or 'libvmaf' in result.stderr:
            print("\nffmpeg に libvmaf がありません。インストール方法:", file=sys.stderr)
            print("  conda install -c conda-forge ffmpeg", file=sys.stderr)
            print("  または ffmpeg をソースビルド (--enable-libvmaf)", file=sys.stderr)
        os.unlink(log_path)
        sys.exit(1)

    with open(log_path, 'r') as f:
        vmaf_log = json.load(f)
    os.unlink(log_path)

    per_frame = []
    scores = []
    ssim_scores = []
    for frame in vmaf_log.get('frames', []):
        metrics = frame.get('metrics', {})
        score = metrics.get('vmaf', 0)
        ssim_val = metrics.get('float_ssim', None)
        scores.append(score)
        if ssim_val is not None:
            ssim_scores.append(ssim_val)
        per_frame.append({
            'frame': frame.get('frameNum', len(per_frame)),
            'vmaf': score,
            'ssim': ssim_val
        })

    if not scores:
        print("エラー: VMAF スコアが取得できませんでした", file=sys.stderr)
        sys.exit(1)

    sorted_scores = sorted(scores)
    n = len(sorted_scores)

    result_dict = {
        'vmaf_mean': sum(scores) / n,
        'vmaf_min': min(scores),
        'vmaf_max': max(scores),
        'vmaf_p5': sorted_scores[max(0, int(n * 0.05))],
        'vmaf_p25': sorted_scores[max(0, int(n * 0.25))],
        'frames': n,
        'per_frame': per_frame
    }
    if ssim_scores:
        result_dict['ssim_mean'] = sum(ssim_scores) / len(ssim_scores)
    return result_dict


# ============================================================
#  メイン: フレーム対応ありの比較パイプライン
# ============================================================

def run_aligned_comparison(args, width: int, height: int, fps: int,
                           alignment: FrameAlignment) -> dict:
    """
    CSV に基づいてフレーム対応を取り、対応するフレームだけで VMAF を計算する。

    処理の流れ:
      1. alignment.matched_ref_indices で reference から対応フレームを抽出
      2. alignment.matched_dist_indices で distorted から対応フレームを抽出
      3. 抽出した YUV 同士で VMAF を計算
    """
    print(f"\n--- フレーム対応情報 ---")
    print(f"  Sunshine 録画フレーム数:   {alignment.ref_total}")
    print(f"  Moonlight 録画フレーム数:  {alignment.dist_total}")
    print(f"  対応済みフレーム数:        {len(alignment.matched_frame_nrs)}")
    print(f"  ネットワークドロップ:      {len(alignment.dropped_frame_nrs)}")
    if alignment.dropped_frame_nrs:
        dropped_display = alignment.dropped_frame_nrs[:20]
        suffix = f" ... (他 {len(alignment.dropped_frame_nrs) - 20} 件)" \
            if len(alignment.dropped_frame_nrs) > 20 else ""
        print(f"  ドロップした frame_nr:     {dropped_display}{suffix}")

    if not alignment.matched_frame_nrs:
        print("エラー: 対応するフレームがありません", file=sys.stderr)
        sys.exit(1)

    # 一時 YUV ファイルに対応フレームを抽出
    tmp_ref = tempfile.NamedTemporaryFile(suffix='_ref.yuv', delete=False)
    tmp_dist = tempfile.NamedTemporaryFile(suffix='_dist.yuv', delete=False)
    tmp_ref_path = tmp_ref.name
    tmp_dist_path = tmp_dist.name
    tmp_ref.close()
    tmp_dist.close()

    try:
        # Reference (Sunshine) のフレーム抽出
        print(f"\n  Reference からフレーム抽出中...")
        ref_type = get_file_type(args.reference)
        if ref_type == 'container':
            ref_written = extract_container_frames(
                args.reference, alignment.matched_ref_indices,
                tmp_ref_path, width, height)
            ref_pixfmt_for_vmaf = 'yuv420p'  # コンテナからの抽出は YUV420P
        else:
            ref_written = extract_raw_frames(
                args.reference, alignment.matched_ref_indices,
                tmp_ref_path, width, height,
                args.ref_pixfmt or 'yuv420p')
            ref_pixfmt_for_vmaf = args.ref_pixfmt or 'yuv420p'

        # Distorted (Moonlight) のフレーム抽出
        print(f"  Distorted からフレーム抽出中...")
        dist_type = get_file_type(args.distorted)
        if dist_type == 'container':
            dist_written = extract_container_frames(
                args.distorted, alignment.matched_dist_indices,
                tmp_dist_path, width, height)
            dist_pixfmt_for_vmaf = 'yuv420p'
        else:
            dist_written = extract_raw_frames(
                args.distorted, alignment.matched_dist_indices,
                tmp_dist_path, width, height,
                args.dist_pixfmt or 'yuv420p')
            dist_pixfmt_for_vmaf = args.dist_pixfmt or 'yuv420p'

        print(f"  Reference 抽出: {ref_written} フレーム")
        print(f"  Distorted 抽出: {dist_written} フレーム")

        if ref_written != dist_written:
            print(f"警告: 抽出フレーム数が一致しません "
                  f"(ref={ref_written}, dist={dist_written})", file=sys.stderr)

        # 抽出した YUV で VMAF 計算
        frames_limit = min(ref_written, dist_written)
        if args.frames > 0:
            frames_limit = min(frames_limit, args.frames)

        print(f"\n  VMAF 計算中 ({frames_limit} フレーム)...")

        results = run_vmaf(
            tmp_ref_path, tmp_dist_path,
            width, height, fps,
            ref_pixfmt_for_vmaf, dist_pixfmt_for_vmaf,
            frames_limit, args.verbose,
            args.model_path, args.model_version
        )

        # frame_nr を per_frame に紐づける
        for i, pf in enumerate(results['per_frame']):
            if i < len(alignment.matched_frame_nrs):
                pf['frame_nr'] = alignment.matched_frame_nrs[i]

        # ドロップ情報を結果に追加
        results['alignment'] = {
            'ref_total': alignment.ref_total,
            'dist_total': alignment.dist_total,
            'matched': len(alignment.matched_frame_nrs),
            'dropped': len(alignment.dropped_frame_nrs),
            'dropped_frame_nrs': alignment.dropped_frame_nrs,
        }

        return results

    finally:
        # 一時ファイルの削除
        for p in (tmp_ref_path, tmp_dist_path):
            if os.path.exists(p):
                os.unlink(p)


# ============================================================
#  結果表示・保存
# ============================================================

def print_results(results: dict, alignment: Optional[FrameAlignment] = None):
    vmaf = results['vmaf_mean']
    print()
    print("=" * 55)
    print("  VMAF 結果")
    print("=" * 55)
    print(f"  平均:       {vmaf:.4f}")
    print(f"  最小:       {results['vmaf_min']:.2f}")
    print(f"  最大:       {results['vmaf_max']:.2f}")
    print(f"  5th pctl:   {results['vmaf_p5']:.2f}")
    print(f"  25th pctl:  {results['vmaf_p25']:.2f}")
    if 'ssim_mean' in results:
        print(f"  SSIM:       {results['ssim_mean']:.6f}")
    print(f"  フレーム数: {results['frames']}")

    if alignment:
        drop_rate = len(alignment.dropped_frame_nrs) / max(1, alignment.ref_total) * 100
        print(f"  ドロップ率: {drop_rate:.2f}% "
              f"({len(alignment.dropped_frame_nrs)}/{alignment.ref_total})")

    print("=" * 55)


def save_log(args, results: dict, width: int, height: int, fps: int):
    if not args.log_file:
        return

    lines = [
        f"timestamp={datetime.now().isoformat(timespec='seconds')}",
        f"reference={args.reference}",
        f"distorted={args.distorted}",
        f"width={width}",
        f"height={height}",
        f"fps={fps}",
        f"ref_pixfmt={args.ref_pixfmt or 'auto'}",
        f"dist_pixfmt={args.dist_pixfmt or 'auto'}",
        f"frames={results['frames']}",
        f"vmaf_mean={results['vmaf_mean']:.4f}",
        f"vmaf_min={results['vmaf_min']:.4f}",
        f"vmaf_max={results['vmaf_max']:.4f}",
        f"vmaf_p5={results['vmaf_p5']:.4f}",
        f"vmaf_p25={results['vmaf_p25']:.4f}",
    ]
    if 'ssim_mean' in results:
        lines.append(f"ssim_mean={results['ssim_mean']:.6f}")

    # フレーム対応情報
    if 'alignment' in results:
        a = results['alignment']
        lines.append(f"alignment_matched={a['matched']}")
        lines.append(f"alignment_dropped={a['dropped']}")
        if a['dropped_frame_nrs']:
            lines.append(f"dropped_frame_nrs={a['dropped_frame_nrs'][:50]}")

    lines.append("-" * 40)

    if args.log_per_frame:
        lines.append("per_frame_metrics=")
        for pf in results['per_frame']:
            fnr = pf.get('frame_nr', '')
            fnr_str = f", frame_nr={fnr}" if fnr != '' else ''
            lines.append(
                "  frame={frame}, vmaf={v:.4f}, ssim={s}{fnr}".format(
                    frame=pf['frame'],
                    v=pf['vmaf'],
                    s=f"{pf['ssim']:.6f}" if pf['ssim'] is not None else "n/a",
                    fnr=fnr_str,
                )
            )
        lines.append("-" * 40)

    with open(args.log_file, 'a', encoding='utf-8') as f:
        f.write("\n".join(lines) + "\n")


def save_json(args, results: dict):
    if not args.output:
        return

    output = {
        'reference': args.reference,
        'distorted': args.distorted,
        'vmaf_mean': results['vmaf_mean'],
        'vmaf_min': results['vmaf_min'],
        'vmaf_max': results['vmaf_max'],
        'vmaf_p5': results['vmaf_p5'],
        'vmaf_p25': results['vmaf_p25'],
        'ssim_mean': results.get('ssim_mean'),
        'frames': results['frames'],
        'per_frame': results['per_frame'],
    }
    if 'alignment' in results:
        output['alignment'] = results['alignment']

    with open(args.output, 'w') as f:
        json.dump(output, f, indent=2, ensure_ascii=False)
    print(f"保存: {args.output}")


# ============================================================
#  main
# ============================================================

def main():
    parser = argparse.ArgumentParser(
        description='Sunshine vs Moonlight VMAF 比較（フレーム対応版）')

    # 入力ファイル
    parser.add_argument('--reference', '-r', required=True,
                        help='Sunshine 側の録画 (.mkv/.mp4/.yuv)')
    parser.add_argument('--distorted', '-d', required=True,
                        help='Moonlight 側の録画 (.yuv)')

    # フレーム対応 CSV（オプション）
    parser.add_argument('--ref-csv', default='',
                        help='Sunshine 側のフレーム CSV (frame_nr 入り)')
    parser.add_argument('--dist-csv', default='',
                        help='Moonlight 側のフレーム CSV (frame_nr 入り)')

    # 映像パラメータ
    parser.add_argument('--width', '-W', type=int, default=0)
    parser.add_argument('--height', '-H', type=int, default=0)
    parser.add_argument('--fps', type=int, default=0)
    parser.add_argument('--ref-pixfmt', default='', help='Reference の pixfmt (raw 時)')
    parser.add_argument('--dist-pixfmt', default='', help='Distorted の pixfmt (raw 時)')
    parser.add_argument('--meta', '-m', default='', help='.meta ファイル')

    # 出力
    parser.add_argument('--frames', '-n', type=int, default=0,
                        help='比較フレーム数 (0=全部)')
    parser.add_argument('--output', '-o', default='', help='JSON 出力先')
    parser.add_argument('--log-file', '-l', default='',
                        help='結果を追記保存するログファイルパス')
    parser.add_argument('--log-per-frame', action='store_true',
                        help='フレームごとの VMAF をログにも書き出す')
    parser.add_argument('--verbose', '-v', action='store_true')
    parser.add_argument('--model-path', default='',
                        help='VMAF モデル: ファイルパス (/path/to/vmaf_v0.6.1.json) '
                             'または built-in バージョン名 (vmaf_b_v0.6.3)')
    parser.add_argument('--model-version', default='v2', choices=['v1', 'v2'],
                        help='libvmaf のオプション書式 (v2=ffmpeg5+/デフォルト, v1=ffmpeg4)')

    args = parser.parse_args()

    # ファイル確認
    for f in [args.reference, args.distorted]:
        if not os.path.isfile(f):
            print(f"エラー: ファイルが見つかりません: {f}", file=sys.stderr)
            sys.exit(1)

    for f in [args.ref_csv, args.dist_csv]:
        if f and not os.path.isfile(f):
            print(f"エラー: CSV ファイルが見つかりません: {f}", file=sys.stderr)
            sys.exit(1)

    # .meta からパラメータ取得
    width, height, fps = args.width, args.height, args.fps
    meta_path = args.meta

    if meta_path and not os.path.isfile(meta_path):
        ref_path = Path(args.reference)
        candidate = ref_path.with_suffix(ref_path.suffix + '.meta')
        if candidate.is_file():
            meta_path = str(candidate)

    if meta_path and os.path.isfile(meta_path):
        meta = parse_meta_file(meta_path)
        width = width or int(meta.get('width', 0))
        height = height or int(meta.get('height', 0))
        fps = fps or int(meta.get('fps', 0))
        if not args.ref_pixfmt:
            args.ref_pixfmt = meta.get('source_pix_fmt', '')

    # raw ファイルの場合は解像度必須
    needs_dims = (get_file_type(args.reference) == 'rawvideo' or
                  get_file_type(args.distorted) == 'rawvideo')
    if needs_dims and (not width or not height or not fps):
        print("エラー: raw ファイルには --width, --height, --fps が必要です",
              file=sys.stderr)
        sys.exit(1)

    # 実行
    print(f"VMAF 計算中...")
    print(f"  Reference: {args.reference}")
    print(f"  Distorted: {args.distorted}")

    alignment = None

    if args.ref_csv and args.dist_csv:
        # ===== CSV ありモード: フレーム対応を取って比較 =====
        print(f"  Ref CSV:   {args.ref_csv}")
        print(f"  Dist CSV:  {args.dist_csv}")

        alignment = align_frames(args.ref_csv, args.dist_csv)
        results = run_aligned_comparison(args, width, height, fps, alignment)
    else:
        # ===== CSV なしモード: 従来互換 =====
        results = run_vmaf(
            args.reference, args.distorted, width, height, fps,
            args.ref_pixfmt, args.dist_pixfmt, args.frames, args.verbose,
            args.model_path, args.model_version
        )

    # 結果表示
    print_results(results, alignment)

    # 保存
    save_log(args, results, width, height, fps)
    save_json(args, results)


if __name__ == '__main__':
    main()
