#!/usr/bin/env python3
"""Print what fraction of its app partition the built firmware image uses.

`idf.py build` reports the app binary's byte count but never a percentage:
ESP-IDF 6.0's size tool (`esp_idf_size`) has no partition-table awareness, so
it can't tell how full the app partition actually is. This asks ESP-IDF's own
`gen_esp32part.py` to dump the partition table it just built (reusing its
parser instead of re-parsing partitions.csv by hand, and reflecting whatever
alignment IDF actually applied) and compares the app binary against the app
partition's size.

Usage:
    report_app_size.py build/wt32-bridge.bin build/partition_table/partition-table.bin
    report_app_size.py build/wt32-bridge.bin build/partition_table/partition-table.bin --idf-path $IDF_PATH
"""

import argparse
import csv
import json
import os
import subprocess
import sys


def find_idf_path(app_bin):
    # Same discovery rule as CLAUDE.md: project_description.json in the build
    # dir is authoritative, since it's what actually produced this binary.
    project_description = os.path.join(os.path.dirname(app_bin), "project_description.json")
    with open(project_description) as f:
        return json.load(f)["idf_path"]


def parse_size(text):
    text = text.strip()
    if text.startswith("0x"):
        return int(text, 16)
    if text and text[-1] in "KM":
        multiplier = 1024 if text[-1] == "K" else 1024 * 1024
        return int(text[:-1]) * multiplier
    return int(text)


def get_app_partition(idf_path, partition_table_bin):
    gen_esp32part = os.path.join(idf_path, "components", "partition_table", "gen_esp32part.py")
    output = subprocess.run(
        [sys.executable, gen_esp32part, partition_table_bin],
        check=True,
        capture_output=True,
        text=True,
    ).stdout

    rows = [row for row in csv.reader(output.splitlines()) if row and not row[0].startswith("#")]
    for name, part_type, _subtype, _offset, size, *_rest in rows:
        if part_type.strip() == "app":
            # ota_0/ota_1 are expected to be the same size; the first app
            # partition found is representative either way.
            return name.strip(), parse_size(size)

    sys.exit("report_app_size.py: no app-type partition found in partition table")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("app_bin", help="Path to the built app binary (e.g. build/wt32-bridge.bin)")
    parser.add_argument("partition_table_bin", help="Path to the built partition-table.bin")
    parser.add_argument(
        "--idf-path",
        help="ESP-IDF root, for locating gen_esp32part.py "
        "(default: read from project_description.json next to app_bin)",
    )
    args = parser.parse_args()

    idf_path = args.idf_path or find_idf_path(args.app_bin)
    app_size = os.path.getsize(args.app_bin)
    part_name, part_size = get_app_partition(idf_path, args.partition_table_bin)

    pct = app_size / part_size * 100
    print(f"App image: {app_size} / {part_size} bytes ({pct:.1f}%) of app partition ({part_name})")


if __name__ == "__main__":
    main()
