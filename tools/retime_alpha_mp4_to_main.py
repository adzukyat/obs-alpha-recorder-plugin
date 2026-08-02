#!/usr/bin/env python3
"""Offline PoC: retime Alpha Recorder MP4 samples to match a main video track.

This intentionally handles the simple files produced by Alpha Recorder's direct
MP4 muxer: one video track, one chunk, HEVC samples, optional edit list, and no
alpha B-frame CTTS. It copies alpha ftyp/mdat bytes unchanged and rebuilds moov.
"""

from __future__ import annotations

import argparse
import dataclasses
import json
import math
import struct
import subprocess
from pathlib import Path
from typing import Iterable


CONTAINER_BOXES = {b"moov", b"trak", b"mdia", b"minf", b"stbl", b"edts"}


def u32(data: bytes, offset: int) -> int:
    return struct.unpack_from(">I", data, offset)[0]


def i32(data: bytes, offset: int) -> int:
    return struct.unpack_from(">i", data, offset)[0]


def u64(data: bytes, offset: int) -> int:
    return struct.unpack_from(">Q", data, offset)[0]


def i64(data: bytes, offset: int) -> int:
    return struct.unpack_from(">q", data, offset)[0]


def put_u32(value: int) -> bytes:
    return struct.pack(">I", value)


def put_i32(value: int) -> bytes:
    return struct.pack(">i", value)


def put_u64(value: int) -> bytes:
    return struct.pack(">Q", value)


def put_i64(value: int) -> bytes:
    return struct.pack(">q", value)


@dataclasses.dataclass
class Box:
    path: tuple[bytes, ...]
    start: int
    header: int
    end: int

    @property
    def typ(self) -> bytes:
        return self.path[-1]

    @property
    def payload_start(self) -> int:
        return self.start + self.header

    @property
    def size(self) -> int:
        return self.end - self.start


@dataclasses.dataclass
class ParsedTrack:
    mdhd_timescale: int
    mdhd_duration: int
    tkhd_duration: int
    stts_durations: list[int]
    stsz_sizes: list[int]
    stss_samples: list[int]
    stsc_samples_per_chunk: int
    co_box: bytes
    elst_segment_duration: int | None
    elst_media_time: int | None


def iter_boxes(data: bytes, start: int = 0, end: int | None = None, path: tuple[bytes, ...] = ()) -> Iterable[Box]:
    if end is None:
        end = len(data)
    offset = start
    while offset + 8 <= end:
        size = u32(data, offset)
        typ = data[offset + 4 : offset + 8]
        header = 8
        if size == 1:
            if offset + 16 > end:
                break
            size = u64(data, offset + 8)
            header = 16
        elif size == 0:
            size = end - offset
        if size < header or offset + size > end:
            break
        box = Box(path + (typ,), offset, header, offset + size)
        yield box
        if typ in CONTAINER_BOXES:
            yield from iter_boxes(data, offset + header, offset + size, path + (typ,))
        offset += size


def top_level_boxes(data: bytes) -> list[Box]:
    return [box for box in iter_boxes(data) if len(box.path) == 1]


def box(payload_type: bytes, payload: bytes) -> bytes:
    size = len(payload) + 8
    if size <= 0xFFFFFFFF:
        return put_u32(size) + payload_type + payload
    return put_u32(1) + payload_type + put_u64(size + 8) + payload


def full_box(payload_type: bytes, version: int, flags: int, payload: bytes) -> bytes:
    return box(payload_type, bytes([version]) + flags.to_bytes(3, "big") + payload)


def find_box(data: bytes, path: tuple[bytes, ...]) -> Box | None:
    for found in iter_boxes(data):
        if found.path == path:
            return found
    return None


def find_child_payload(data: bytes, path: tuple[bytes, ...]) -> bytes:
    found = find_box(data, path)
    if found is None:
        raise RuntimeError(f"missing box: {'/'.join(p.decode('latin1') for p in path)}")
    return data[found.payload_start : found.end]


def parse_mvhd(payload: bytes) -> tuple[int, int]:
    version = payload[0]
    if version == 1:
        return u32(payload, 20), u64(payload, 24)
    return u32(payload, 12), u32(payload, 16)


def parse_tkhd_duration(payload: bytes) -> int:
    return u64(payload, 28) if payload[0] == 1 else u32(payload, 20)


def parse_mdhd(payload: bytes) -> tuple[int, int]:
    version = payload[0]
    if version == 1:
        return u32(payload, 20), u64(payload, 24)
    return u32(payload, 12), u32(payload, 16)


def parse_elst(payload: bytes) -> tuple[int, int] | None:
    count = u32(payload, 4)
    if count == 0:
        return None
    version = payload[0]
    offset = 8
    if version == 1:
        return u64(payload, offset), i64(payload, offset + 8)
    return u32(payload, offset), i32(payload, offset + 4)


def parse_stts(payload: bytes) -> list[int]:
    count = u32(payload, 4)
    offset = 8
    durations: list[int] = []
    for _ in range(count):
        sample_count = u32(payload, offset)
        duration = u32(payload, offset + 4)
        offset += 8
        durations.extend([duration] * sample_count)
    return durations


def parse_ctts(payload: bytes | None) -> list[int]:
    if payload is None:
        return []
    version = payload[0]
    count = u32(payload, 4)
    offset = 8
    offsets: list[int] = []
    for _ in range(count):
        sample_count = u32(payload, offset)
        composition_offset = i32(payload, offset + 4) if version == 1 else u32(payload, offset + 4)
        offset += 8
        offsets.extend([composition_offset] * sample_count)
    return offsets


def parse_stsz(payload: bytes) -> list[int]:
    sample_size = u32(payload, 4)
    sample_count = u32(payload, 8)
    if sample_size != 0:
        return [sample_size] * sample_count
    offset = 12
    return [u32(payload, offset + index * 4) for index in range(sample_count)]


def parse_stss(payload: bytes | None) -> list[int]:
    if payload is None:
        return []
    count = u32(payload, 4)
    offset = 8
    return [u32(payload, offset + index * 4) for index in range(count)]


def probe_presentation_durations(path: Path) -> list[int]:
    stream_json = subprocess.check_output(
        [
            "ffprobe",
            "-v",
            "error",
            "-select_streams",
            "v:0",
            "-show_entries",
            "stream=duration_ts",
            "-of",
            "json",
            str(path),
        ],
        text=True,
    )
    packet_json = subprocess.check_output(
        [
            "ffprobe",
            "-v",
            "error",
            "-select_streams",
            "v:0",
            "-show_packets",
            "-show_entries",
            "packet=pts,dts,duration",
            "-of",
            "json",
            str(path),
        ],
        text=True,
    )
    streams = json.loads(stream_json).get("streams", [])
    packets = json.loads(packet_json).get("packets", [])
    if not streams or "duration_ts" not in streams[0]:
        raise RuntimeError("main stream duration_ts is not available")
    duration_ts = int(streams[0]["duration_ts"])
    rows: list[tuple[int, int, int]] = []
    for packet in packets:
        if "pts" not in packet:
            continue
        pts = int(packet["pts"])
        dts = int(packet.get("dts", pts))
        duration = int(packet.get("duration", 0))
        rows.append((pts, dts, duration))
    if not rows:
        raise RuntimeError("main stream has no packet PTS values")
    rows.sort(key=lambda row: (row[0], row[1]))
    durations: list[int] = []
    for index, (pts, _dts, duration) in enumerate(rows):
        if index + 1 < len(rows):
            delta = rows[index + 1][0] - pts
            if delta <= 0:
                raise RuntimeError("main presentation PTS is not strictly increasing")
            durations.append(delta)
        else:
            tail = duration_ts - pts
            durations.append(tail if tail > 0 else duration)
    if any(duration <= 0 for duration in durations):
        raise RuntimeError("main presentation duration contains a non-positive sample")
    return durations


def probe_decode_to_presentation_ranks(path: Path) -> list[int]:
    packet_json = subprocess.check_output(
        [
            "ffprobe",
            "-v",
            "error",
            "-select_streams",
            "v:0",
            "-show_packets",
            "-show_entries",
            "packet=pts,dts",
            "-of",
            "json",
            str(path),
        ],
        text=True,
    )
    rows: list[tuple[int, int, int]] = []
    for decode_index, packet in enumerate(json.loads(packet_json).get("packets", [])):
        if "pts" not in packet:
            continue
        rows.append((int(packet["pts"]), int(packet.get("dts", packet["pts"])), decode_index))
    if not rows:
        raise RuntimeError("main stream has no packet PTS values")
    presentation_order = sorted(rows, key=lambda row: (row[0], row[1], row[2]))
    ranks = [0] * len(rows)
    for presentation_rank, (_pts, _dts, decode_index) in enumerate(presentation_order):
        ranks[decode_index] = presentation_rank
    return ranks


def parse_stsc_samples_per_chunk(payload: bytes) -> int:
    count = u32(payload, 4)
    if count == 0:
        return 0
    return u32(payload, 16)


def parse_video_track(data: bytes) -> ParsedTrack:
    stss_box = find_box(data, (b"moov", b"trak", b"mdia", b"minf", b"stbl", b"stss"))
    elst_box = find_box(data, (b"moov", b"trak", b"edts", b"elst"))
    elst = parse_elst(data[elst_box.payload_start : elst_box.end]) if elst_box else None
    mdhd_timescale, mdhd_duration = parse_mdhd(find_child_payload(data, (b"moov", b"trak", b"mdia", b"mdhd")))
    co64_box = find_box(data, (b"moov", b"trak", b"mdia", b"minf", b"stbl", b"co64"))
    stco_box = find_box(data, (b"moov", b"trak", b"mdia", b"minf", b"stbl", b"stco"))
    co_box = data[(co64_box or stco_box).start : (co64_box or stco_box).end] if (co64_box or stco_box) else b""
    if not co_box:
        raise RuntimeError("missing chunk offset table")
    return ParsedTrack(
        mdhd_timescale=mdhd_timescale,
        mdhd_duration=mdhd_duration,
        tkhd_duration=parse_tkhd_duration(find_child_payload(data, (b"moov", b"trak", b"tkhd"))),
        stts_durations=parse_stts(find_child_payload(data, (b"moov", b"trak", b"mdia", b"minf", b"stbl", b"stts"))),
        stsz_sizes=parse_stsz(find_child_payload(data, (b"moov", b"trak", b"mdia", b"minf", b"stbl", b"stsz"))),
        stss_samples=parse_stss(data[stss_box.payload_start : stss_box.end] if stss_box else None),
        stsc_samples_per_chunk=parse_stsc_samples_per_chunk(
            find_child_payload(data, (b"moov", b"trak", b"mdia", b"minf", b"stbl", b"stsc"))
        ),
        co_box=co_box,
        elst_segment_duration=elst[0] if elst else None,
        elst_media_time=elst[1] if elst else None,
    )


def diagnose_ae_fps(path: Path) -> dict[str, object]:
    data = path.read_bytes()
    movie_timescale, movie_duration = parse_mvhd(find_child_payload(data, (b"moov", b"mvhd")))
    track = parse_video_track(data)
    if not track.stts_durations:
        raise RuntimeError("video track has no stts durations")
    unique_decode_durations = sorted(set(track.stts_durations))
    if len(unique_decode_durations) != 1:
        raise RuntimeError(f"AE-like FPS probe expects CFR stts; found durations={unique_decode_durations[:8]}")
    sample_duration = unique_decode_durations[0]
    if sample_duration <= 0:
        raise RuntimeError("video track has a non-positive stts duration")
    nominal_fps = track.mdhd_timescale / sample_duration
    segment_duration = track.elst_segment_duration if track.elst_segment_duration is not None else track.tkhd_duration
    if segment_duration <= 0 or movie_timescale <= 0:
        raise RuntimeError("movie/edit duration is not positive")
    segment_seconds = segment_duration / movie_timescale

    # macOS After Effects appears to use the nominal stream rate for ordinary
    # tracks, but derive the displayed rate from the final visible frame index
    # over the edit-list duration when a non-zero edit media time is present.
    # This reproduces the observed 59.976/59.977 display for Alpha Recorder
    # MP4s whose nominal stream rate is still 60/1.
    nominal_visible_frames = segment_seconds * nominal_fps
    ae_frame_index = max(0, math.ceil(nominal_visible_frames) - 1)
    ae_edit_list_fps = ae_frame_index / segment_seconds
    has_nonzero_edit = track.elst_media_time is not None and track.elst_media_time != 0
    ae_likely_display_fps = ae_edit_list_fps if has_nonzero_edit else nominal_fps

    return {
        "file": str(path),
        "movie_timescale": movie_timescale,
        "movie_duration": movie_duration,
        "movie_duration_seconds": movie_duration / movie_timescale,
        "tkhd_duration": track.tkhd_duration,
        "elst_segment_duration": track.elst_segment_duration,
        "elst_media_time": track.elst_media_time,
        "mdhd_timescale": track.mdhd_timescale,
        "mdhd_duration": track.mdhd_duration,
        "sample_count": len(track.stts_durations),
        "stts_sample_duration": sample_duration,
        "nominal_fps": nominal_fps,
        "segment_duration_seconds": segment_seconds,
        "nominal_visible_frames": nominal_visible_frames,
        "ae_like_frame_index": ae_frame_index,
        "ae_edit_list_fps": ae_edit_list_fps,
        "ae_likely_display_fps": ae_likely_display_fps,
        "has_nonzero_edit": has_nonzero_edit,
    }


def stts_payload(durations: list[int]) -> bytes:
    entries: list[tuple[int, int]] = []
    for duration in durations:
        if not entries or entries[-1][1] != duration:
            entries.append((1, duration))
        else:
            entries[-1] = (entries[-1][0] + 1, duration)
    payload = put_u32(len(entries))
    for count, duration in entries:
        payload += put_u32(count) + put_u32(duration)
    return payload


def ctts_payload(offsets: list[int]) -> bytes:
    entries: list[tuple[int, int]] = []
    for offset in offsets:
        if not entries or entries[-1][1] != offset:
            entries.append((1, offset))
        else:
            entries[-1] = (entries[-1][0] + 1, offset)
    payload = put_u32(len(entries))
    for count, offset in entries:
        if offset < 0:
            raise RuntimeError("PoC only writes ctts version 0 offsets")
        payload += put_u32(count) + put_u32(offset)
    return payload


def stsz_payload(sizes: list[int]) -> bytes:
    return put_u32(0) + put_u32(len(sizes)) + b"".join(put_u32(size) for size in sizes)


def stss_payload(sync_samples: list[int]) -> bytes:
    return put_u32(len(sync_samples)) + b"".join(put_u32(sample) for sample in sync_samples)


def stsc_box(sample_count: int) -> bytes:
    return full_box(b"stsc", 0, 0, put_u32(1) + put_u32(1) + put_u32(sample_count) + put_u32(1))


def chunk_offset_box(original_box: bytes, first_offset: int) -> bytes:
    typ = original_box[4:8]
    if typ == b"co64":
        return full_box(b"co64", 0, 0, put_u32(1) + put_u64(first_offset))
    if typ == b"stco":
        if first_offset > 0xFFFFFFFF:
            return full_box(b"co64", 0, 0, put_u32(1) + put_u64(first_offset))
        return full_box(b"stco", 0, 0, put_u32(1) + put_u32(first_offset))
    raise RuntimeError("unsupported chunk offset box")


def first_chunk_offset(original_box: bytes) -> int:
    typ = original_box[4:8]
    payload = original_box[8:]
    if typ == b"co64":
        if u32(payload, 4) == 0:
            raise RuntimeError("empty co64")
        return u64(payload, 8)
    if typ == b"stco":
        if u32(payload, 4) == 0:
            raise RuntimeError("empty stco")
        return u32(payload, 8)
    raise RuntimeError("unsupported chunk offset box")


def sample_payloads(data: bytes, first_offset: int, sizes: list[int]) -> list[bytes]:
    offset = first_offset
    payloads: list[bytes] = []
    for size in sizes:
        payloads.append(data[offset : offset + size])
        offset += size
    return payloads


def top_level_box_bytes(data: bytes, typ: bytes) -> bytes:
    for top in top_level_boxes(data):
        if top.typ == typ:
            return data[top.start : top.end]
    raise RuntimeError(f"missing top-level box: {typ.decode('latin1')}")


def mvhd_box(original_payload: bytes, timescale: int, duration: int) -> bytes:
    payload = bytearray(original_payload)
    if payload[0] != 0:
        raise RuntimeError("PoC only rewrites mvhd version 0")
    payload[12:16] = put_u32(timescale)
    payload[16:20] = put_u32(duration)
    return full_box(b"mvhd", payload[0], int.from_bytes(payload[1:4], "big"), bytes(payload[4:]))


def tkhd_box(original_payload: bytes, duration: int) -> bytes:
    payload = bytearray(original_payload)
    if payload[0] != 0:
        raise RuntimeError("PoC only rewrites tkhd version 0")
    payload[20:24] = put_u32(duration)
    return full_box(b"tkhd", payload[0], int.from_bytes(payload[1:4], "big"), bytes(payload[4:]))


def mdhd_box(original_payload: bytes, timescale: int, duration: int) -> bytes:
    payload = bytearray(original_payload)
    if payload[0] != 0:
        raise RuntimeError("PoC only rewrites mdhd version 0")
    payload[12:16] = put_u32(timescale)
    payload[16:20] = put_u32(duration)
    return full_box(b"mdhd", payload[0], int.from_bytes(payload[1:4], "big"), bytes(payload[4:]))


def elst_box(segment_duration: int, media_time: int) -> bytes:
    payload = put_u32(1) + put_u32(segment_duration) + put_i32(media_time) + put_u32(0x00010000)
    return full_box(b"elst", 0, 0, payload)


def rebuild_container(data: bytes, container_box: Box, replacements: dict[tuple[bytes, ...], bytes]) -> bytes:
    payload = b""
    offset = container_box.payload_start
    inserted_ctts = False
    while offset < container_box.end:
        size = u32(data, offset)
        typ = data[offset + 4 : offset + 8]
        header = 8
        if size == 1:
            size = u64(data, offset + 8)
            header = 16
        if size < header or offset + size > container_box.end:
            raise RuntimeError("invalid child box while rebuilding")
        path = container_box.path + (typ,)
        if path in replacements:
            payload += replacements[path]
        elif typ in CONTAINER_BOXES:
            payload += rebuild_container(data, Box(path, offset, header, offset + size), replacements)
        else:
            payload += data[offset : offset + size]
        if (
            container_box.path == (b"moov", b"trak", b"mdia", b"minf", b"stbl")
            and typ == b"stts"
            and (b"moov", b"trak", b"mdia", b"minf", b"stbl", b"ctts") in replacements
            and find_box(data, (b"moov", b"trak", b"mdia", b"minf", b"stbl", b"ctts")) is None
        ):
            payload += replacements[(b"moov", b"trak", b"mdia", b"minf", b"stbl", b"ctts")]
            inserted_ctts = True
        offset += size
    if (
        not inserted_ctts
        and container_box.path == (b"moov", b"trak", b"mdia", b"minf", b"stbl")
        and (b"moov", b"trak", b"mdia", b"minf", b"stbl", b"ctts") in replacements
        and find_box(data, (b"moov", b"trak", b"mdia", b"minf", b"stbl", b"ctts")) is None
    ):
        payload += replacements[(b"moov", b"trak", b"mdia", b"minf", b"stbl", b"ctts")]
    return box(container_box.typ, payload)


def make_retimed_alpha(
    main_path: Path,
    alpha_path: Path,
    duration_source: str,
    drop_prefix: bool,
    timing_source: str,
    content_shift: int,
    tail_mode: str,
    tail_duration_policy: str,
) -> tuple[bytes, dict[str, object]]:
    main_data = main_path.read_bytes()
    alpha_data = alpha_path.read_bytes()
    main_mvhd_timescale, main_mvhd_duration = parse_mvhd(find_child_payload(main_data, (b"moov", b"mvhd")))
    main = parse_video_track(main_data)
    alpha = parse_video_track(alpha_data)
    output_media_timescale = main.mdhd_timescale
    main_durations = (
        probe_presentation_durations(main_path)
        if timing_source == "presentation-pts"
        else main.stts_durations
    )
    main_decode_ctts = timing_source == "main-decode-ctts"
    main_ctts_payload = None
    main_ctts_offsets: list[int] = []
    if main_decode_ctts:
        main_ctts_box = find_box(main_data, (b"moov", b"trak", b"mdia", b"minf", b"stbl", b"ctts"))
        if main_ctts_box is None:
            raise RuntimeError("main-decode-ctts requires a main ctts box")
        main_ctts_payload = main_data[main_ctts_box.payload_start : main_ctts_box.end]
        main_ctts_offsets = parse_ctts(main_ctts_payload)
        main_durations = main.stts_durations
    elif timing_source == "constant-visible-rate":
        visible_media_duration = sum(main.stts_durations)
        visible_samples = len(main.stts_durations)
        gcd = math.gcd(visible_media_duration, main.mdhd_timescale * visible_samples)
        constant_duration = visible_media_duration // gcd
        output_media_timescale = (main.mdhd_timescale * visible_samples) // gcd
        main_durations = [constant_duration] * visible_samples

    visible_count = len(main_durations)
    if alpha.elst_media_time is None:
        prefix_count = 0
    else:
        if alpha.elst_media_time < 0:
            raise RuntimeError("negative alpha edit media_time is unsupported")
        # Alpha Recorder currently writes one alpha sample per media tick.
        prefix_count = alpha.elst_media_time
    kept_count = prefix_count + visible_count
    if kept_count > len(alpha.stsz_sizes):
        raise RuntimeError(f"alpha has only {len(alpha.stsz_sizes)} samples, need {kept_count}")

    nominal_duration = round(output_media_timescale / 60)
    prefix_durations = [nominal_duration] * prefix_count
    reordered_mdat_payload: bytes | None = None
    if main_decode_ctts:
        if visible_count > len(main_ctts_offsets):
            raise RuntimeError("main ctts has fewer samples than stts")
        ranks = probe_decode_to_presentation_ranks(main_path)
        if len(ranks) != visible_count:
            raise RuntimeError("main decode/presentation rank count does not match stts")
        payloads = sample_payloads(alpha_data, first_chunk_offset(alpha.co_box), alpha.stsz_sizes)
        reordered_payloads = [payloads[prefix_count + rank] for rank in ranks]
        reordered_mdat_payload = b"".join(reordered_payloads)
        new_durations = main.stts_durations[:visible_count]
        new_sizes = [len(payload) for payload in reordered_payloads]
        new_sync = list(range(1, visible_count + 1))
        prefix_duration = int(main.elst_media_time or 0)
    elif drop_prefix:
        new_durations = main_durations[:visible_count]
        new_sizes = alpha.stsz_sizes[prefix_count:kept_count]
        new_sync = [
            sample - prefix_count
            for sample in alpha.stss_samples
            if prefix_count < sample <= kept_count
        ]
    else:
        new_durations = prefix_durations + main_durations[:visible_count]
        new_sizes = alpha.stsz_sizes[:kept_count]
        new_sync = [sample for sample in alpha.stss_samples if sample <= kept_count]
    if not new_sync:
        new_sync = list(range(1, len(new_sizes) + 1))
    if content_shift and reordered_mdat_payload is None:
        payloads = sample_payloads(alpha_data, first_chunk_offset(alpha.co_box), alpha.stsz_sizes)
        base_indices = list(range(prefix_count, kept_count))
        shifted_payloads = []
        for index in range(len(base_indices)):
            source_index = min(max(index + content_shift, 0), len(base_indices) - 1)
            shifted_payloads.append(payloads[base_indices[source_index]])
        reordered_mdat_payload = b"".join(shifted_payloads)
        new_sizes = [len(payload) for payload in shifted_payloads]
        new_sync = list(range(1, len(new_sizes) + 1))
    if tail_mode != "none":
        if not new_durations or not new_sizes:
            raise RuntimeError("cannot apply tail mode to an empty sample table")
        tail_duration = new_durations[-1]
        if tail_mode == "extend-last-duration":
            new_durations[-1] += tail_duration
        elif tail_mode == "duplicate-last-sample":
            if reordered_mdat_payload is None:
                payloads = sample_payloads(alpha_data, first_chunk_offset(alpha.co_box), alpha.stsz_sizes)
                if drop_prefix or main_decode_ctts:
                    base_indices = list(range(prefix_count, kept_count))
                else:
                    base_indices = list(range(0, kept_count))
                sample_payload_list = [payloads[index] for index in base_indices]
            else:
                sample_payload_list = sample_payloads(
                    box(b"mdat", reordered_mdat_payload),
                    8,
                    new_sizes,
                )
            sample_payload_list.append(sample_payload_list[-1])
            reordered_mdat_payload = b"".join(sample_payload_list)
            new_sizes.append(new_sizes[-1])
            new_durations.append(tail_duration)
            new_sync = list(range(1, len(new_sizes) + 1))
        else:
            raise RuntimeError(f"unknown tail mode: {tail_mode}")

    if not main_decode_ctts:
        prefix_duration = sum(prefix_durations)
    visible_duration = sum(main_durations[:visible_count])
    if main_decode_ctts:
        track_duration = main.mdhd_duration
    else:
        track_duration = visible_duration if drop_prefix else prefix_duration + visible_duration
    if tail_mode == "extend-last-duration":
        track_duration += new_durations[-1] - (main_durations[visible_count - 1] if visible_count > 0 else 0)
    elif tail_mode == "duplicate-last-sample":
        track_duration += new_durations[-1]
    extended_track_duration = track_duration
    if tail_duration_policy in ("keep-movie", "keep-movie-and-track"):
        track_duration = visible_duration if drop_prefix else prefix_duration + visible_duration
    movie_timescale = main_mvhd_timescale
    if duration_source == "main-track":
        movie_duration = main.tkhd_duration or main_mvhd_duration
    elif duration_source == "visible-floor":
        movie_duration = visible_duration * movie_timescale // output_media_timescale
    elif duration_source == "visible-round":
        movie_duration = (visible_duration * movie_timescale + output_media_timescale // 2) // output_media_timescale
    else:
        raise RuntimeError(f"unknown duration source: {duration_source}")
    if tail_mode == "extend-last-duration":
        movie_duration += (tail_duration * movie_timescale + output_media_timescale // 2) // output_media_timescale
    elif tail_mode == "duplicate-last-sample":
        movie_duration += (new_durations[-1] * movie_timescale + output_media_timescale // 2) // output_media_timescale
    extended_movie_duration = movie_duration
    if tail_duration_policy in ("keep-movie", "keep-movie-and-track"):
        if duration_source == "main-track":
            movie_duration = main.tkhd_duration or main_mvhd_duration
        elif duration_source == "visible-floor":
            movie_duration = visible_duration * movie_timescale // output_media_timescale
        elif duration_source == "visible-round":
            movie_duration = (visible_duration * movie_timescale + output_media_timescale // 2) // output_media_timescale
    if tail_duration_policy == "keep-movie":
        track_duration = extended_track_duration

    moov_box = find_box(alpha_data, (b"moov",))
    if moov_box is None:
        raise RuntimeError("alpha has no moov")
    first_offset = first_chunk_offset(alpha.co_box)
    if main_decode_ctts or content_shift or tail_mode != "none":
        ftyp = top_level_box_bytes(alpha_data, b"ftyp")
        first_offset = len(ftyp) + 8
    elif drop_prefix:
        first_offset += sum(alpha.stsz_sizes[:prefix_count])

    replacements = {
        (b"moov", b"mvhd"): mvhd_box(find_child_payload(alpha_data, (b"moov", b"mvhd")), movie_timescale, movie_duration),
        (b"moov", b"trak", b"tkhd"): tkhd_box(find_child_payload(alpha_data, (b"moov", b"trak", b"tkhd")), movie_duration),
        (b"moov", b"trak", b"mdia", b"mdhd"): mdhd_box(
            find_child_payload(alpha_data, (b"moov", b"trak", b"mdia", b"mdhd")),
            output_media_timescale,
            track_duration,
        ),
        (b"moov", b"trak", b"mdia", b"minf", b"stbl", b"stts"): full_box(b"stts", 0, 0, stts_payload(new_durations)),
        (b"moov", b"trak", b"mdia", b"minf", b"stbl", b"stss"): full_box(b"stss", 0, 0, stss_payload(new_sync)),
        (b"moov", b"trak", b"mdia", b"minf", b"stbl", b"stsc"): stsc_box(len(new_sizes)),
        (b"moov", b"trak", b"mdia", b"minf", b"stbl", b"stsz"): full_box(b"stsz", 0, 0, stsz_payload(new_sizes)),
    }
    if main_decode_ctts:
        replacements[(b"moov", b"trak", b"mdia", b"minf", b"stbl", b"ctts")] = full_box(
            b"ctts", 0, 0, ctts_payload(main_ctts_offsets[:visible_count])
        )
    replacements[(b"moov", b"trak", b"mdia", b"minf", b"stbl", b"co64")] = chunk_offset_box(alpha.co_box, first_offset)
    replacements[(b"moov", b"trak", b"mdia", b"minf", b"stbl", b"stco")] = chunk_offset_box(alpha.co_box, first_offset)
    if main_decode_ctts:
        replacements[(b"moov", b"trak", b"edts", b"elst")] = elst_box(movie_duration, prefix_duration)
    elif drop_prefix:
        replacements[(b"moov", b"trak", b"edts")] = b""
    else:
        replacements[(b"moov", b"trak", b"edts", b"elst")] = elst_box(movie_duration, prefix_duration)
    new_moov = rebuild_container(alpha_data, moov_box, replacements)
    output = bytearray()
    if reordered_mdat_payload is not None:
        output += top_level_box_bytes(alpha_data, b"ftyp")
        output += box(b"mdat", reordered_mdat_payload)
    else:
        for top in top_level_boxes(alpha_data):
            if top.typ == b"moov":
                continue
            output += alpha_data[top.start : top.end]
    output += new_moov
    return bytes(output), {
        "main_visible_samples": visible_count,
        "alpha_prefix_samples": prefix_count,
        "alpha_kept_samples": len(new_sizes),
        "drop_prefix": drop_prefix,
        "movie_timescale": movie_timescale,
        "movie_duration": movie_duration,
        "extended_movie_duration": extended_movie_duration,
        "duration_source": duration_source,
        "timing_source": timing_source,
        "content_shift": content_shift,
        "tail_mode": tail_mode,
        "tail_duration_policy": tail_duration_policy,
        "media_timescale": output_media_timescale,
        "media_duration": track_duration,
        "extended_media_duration": extended_track_duration,
        "visible_media_duration": visible_duration,
        "edit_media_time": prefix_duration,
        "first_chunk_offset": first_offset,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--diagnose-ae-fps",
        type=Path,
        help="Print a read-only MP4 timing summary including the macOS After Effects-like displayed FPS estimate.",
    )
    parser.add_argument("--main", type=Path)
    parser.add_argument("--alpha", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument(
        "--duration-source",
        default="visible-floor",
        choices=("main-track", "visible-floor", "visible-round"),
        help="Movie/edit-list duration source for the retimed alpha track.",
    )
    parser.add_argument(
        "--timing-source",
        default="decode-stts",
        choices=("decode-stts", "presentation-pts", "main-decode-ctts", "constant-visible-rate"),
        help="Sample duration source for the alpha track.",
    )
    parser.add_argument(
        "--drop-prefix",
        action="store_true",
        help="Drop alpha pre-roll from the sample table and remove the edit list.",
    )
    parser.add_argument(
        "--content-shift",
        default=0,
        type=int,
        help="Shift visible alpha sample payloads while keeping timing unchanged.",
    )
    parser.add_argument(
        "--tail-mode",
        default="none",
        choices=("none", "extend-last-duration", "duplicate-last-sample"),
        help="Adjust the tail without changing the leading content phase.",
    )
    parser.add_argument(
        "--tail-duration-policy",
        default="extend-all",
        choices=("extend-all", "keep-movie", "keep-movie-and-track"),
        help="Choose whether a tail pad also extends movie/track durations.",
    )
    args = parser.parse_args()

    if args.diagnose_ae_fps is not None:
        print(json.dumps(diagnose_ae_fps(args.diagnose_ae_fps), indent=2, sort_keys=True))
        return 0

    if args.main is None or args.alpha is None or args.output is None:
        parser.error("--main, --alpha, and --output are required unless --diagnose-ae-fps is used")

    output, stats = make_retimed_alpha(
        args.main,
        args.alpha,
        args.duration_source,
        args.drop_prefix,
        args.timing_source,
        args.content_shift,
        args.tail_mode,
        args.tail_duration_policy,
    )
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(output)
    for key, value in stats.items():
        print(f"{key}={value}")
    print(f"output={args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
