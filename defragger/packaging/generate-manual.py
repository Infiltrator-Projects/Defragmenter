#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build Defragmenter's illustrated, searchable user manual and GPL appendix.

Produces deterministic PDF 1.4 without third-party libraries or additional
installed runtime dependencies. Fonts use the same PDF Type1 base faces as
the independently released filesystem manuals.
"""
from pathlib import Path
import re
import sys


INK = (.060, .115, .200)
MUTED = (.320, .390, .450)
TEAL = (.020, .520, .560)
PALE = (.930, .960, .970)


def colour(rgb):
    return " ".join(str(n) for n in rgb)


def esc(text):
    return text.replace("\\", "\\\\").replace("(", "\\(").replace(")", "\\)")


class Page:
    def __init__(self, title, version, number):
        self.parts = []
        self.rect(0, 750, 595, 92, INK)
        self.rect(45, 747, 90, 4, TEAL)
        self.text(45, 803, "INFILTRATOR  /  APPLICATIONS", 10, "F2", (.570, .820, .870))
        self.text(45, 774, title, 20, "F2", (1, 1, 1))
        self.parts.append("0.810 0.850 0.880 RG 1 w 45 50 m 550 50 l S")
        self.text(45, 37, f"Defragmenter  /  {version}  /  User manual", 9, "F1", MUTED)
        self.text(514, 37, f"{number:02d} / 03", 9, "F1", MUTED)

    def rect(self, x, y, w, h, rgb):
        self.parts.append(f"{colour(rgb)} rg {x} {y} {w} {h} re f")

    def text(self, x, y, value, size=10, font="F1", rgb=INK):
        value.encode("latin-1")
        self.parts.append(
            f"BT /{font} {size} Tf {colour(rgb)} rg "
            f"1 0 0 1 {x} {y} Tm ({esc(value)}) Tj ET"
        )

    def section(self, title, y):
        self.rect(45, y - 4, 4, 22, TEAL)
        self.text(60, y + 7, title, 15, "F2")

    def bytes(self):
        return ("\n".join(self.parts) + "\n").encode("latin-1")


def guide(version):
    pages = []
    p = Page("Safe filesystem maintenance", version, 1)
    p.section("What the application does", 705)
    p.text(45, 678, "Analyse disk allocation and fragmentation without writing to the device.", 11)
    p.text(45, 658, "Where qualified, reorganise supported unmounted layouts and verify results.", 11)
    p.text(45, 638, "Recover uses valid durable recovery state, not an assumed repair.", 11)
    p.section("Allocation map: simplified illustration", 594)
    p.text(45, 567, "Blocks illustrate fragmentation; they do not represent real sectors.", 10, "F1", MUTED)
    p.text(45, 534, "BEFORE", 10, "F2", MUTED)
    for i in range(12):
        p.rect(111 + i * 36, 522, 29, 27,
               TEAL if i in (0, 3, 5, 8, 10) else (.72, .80, .83))
    p.text(45, 477, "AFTER", 10, "F2", MUTED)
    for i in range(12):
        p.rect(111 + i * 36, 465, 29, 27, TEAL if i < 5 else (.72, .80, .83))
    p.section("A controlled operation", 407)
    p.rect(45, 313, 506, 72, PALE)
    for i, label in enumerate(("ANALYSE", "PLAN", "VERIFY", "WRITE / RESTORE")):
        x = 58 + i * 125
        p.rect(x, 343, 112, 30, TEAL)
        p.text(x + 9, 354, label, 9, "F2", (1, 1, 1))
    p.text(58, 326, "Write paths require an exact target and an admitted, verifiable plan.")
    p.section("Important boundaries", 261)
    p.text(45, 232, "Mounted, unsupported or inconsistent targets are refused.", 11)
    p.text(45, 210, "Independent filesystem .so modules must pass identity and ABI checks.", 11)
    p.text(45, 188, "ZFS/OpenZFS and Linux swap are analysis-only.", 11, "F2", (.65, .35, .15))
    p.text(45, 102, "This guide does not establish physical-media recovery qualification.", 10, "F1", MUTED)
    pages.append(p.bytes())

    p = Page("Operations and safety", version, 2)
    p.section("Select the operation", 708)
    operations = (
        ("ANALYSE / MAP", "Read-only inspection of supported filesystem allocations."),
        ("DEFRAGMENT", "Move supported allocations into a verified canonical layout."),
        ("GROWTH DEFRAG", "Reserve exactly 10% post-file free space when admitted."),
        ("RECOVER", "Resume or restore a supported interrupted transaction."),
    )
    for i, (title, description) in enumerate(operations):
        y = 606 - 77 * i
        p.rect(45, y, 506, 64, PALE)
        p.rect(45, y, 4, 64, TEAL)
        p.text(61, y + 40, f"0{i+1}  {title}", 12, "F2")
        p.text(61, y + 17, description, 10.5)
    p.section("Preflight, in order", 340)
    p.text(45, 311, "1. Select and confirm the exact target identity and capacity.", 10.5)
    p.text(45, 290, "2. Reject mounted or overlapping devices and unsafe geometry.", 10.5)
    p.text(45, 269, "3. Verify the external module, layout and recovery prerequisites.", 10.5)
    p.text(45, 248, "4. Confirm operations and independently verify changes.", 10.5)
    p.section("Test Media destroys target contents", 213)
    p.rect(45, 112, 506, 77, (.98, .95, .90))
    p.text(61, 166, "Use only sacrificial disks or test images.", 12, "F2", (.65, .35, .15))
    p.text(61, 145, "The separate 21-slot Test Media workflow is intentionally destructive.")
    p.text(61, 128, "Do not select system disks or irreplaceable storage devices.")
    pages.append(p.bytes())

    p = Page("Installation and ownership", version, 3)
    p.section("Two applications, one user manual", 707)
    p.rect(45, 594, 506, 91, PALE)
    p.text(59, 661, "/Programs/defragmenter", 11, "F3")
    p.text(59, 641, "/Programs/defragmenter-test-media", 11, "F3")
    p.text(59, 611, "/System/Resources/Documentation/Defragmenter.pdf", 10, "F3")
    p.section("Trusted component boundaries", 565)
    p.text(45, 538, "Filesystem drivers are independently owned .so files in", 10.5)
    p.text(45, 520, "/System/Filesystems/ with verified entry points.", 10.5)
    p.text(45, 489, "Private helpers belong to /System/Commands/Defragmenter.", 10.5)
    p.section("Installation and upgrades", 450)
    p.text(45, 422, "Install the published .deb through APT, or use the verified", 10.5)
    p.text(45, 403, "source-bearing .run to compile the CPU-native Debian package.", 10.5)
    p.text(45, 376, "Both paths use Debian ownership, versions and checksum checks.", 10.5)
    p.section("Old documentation folder", 335)
    p.text(45, 306, "Previous packages installed README.md and COPYING.GPL-3.0", 10.5)
    p.text(45, 288, "in a Defragmenter subfolder. Debian retires those old files.", 10.5)
    p.text(45, 270, "Only an empty legacy folder is removed; user files are preserved.", 10.5)
    p.section("Licence and source", 221)
    p.rect(45, 111, 506, 89, PALE)
    p.text(60, 177, "GPL-3.0-or-later", 13, "F2")
    p.text(60, 153, "The complete GNU General Public License v3 follows this guide.", 10)
    p.text(60, 132, "Editable manual source and engineering documentation stay in Git.", 10)
    pages.append(p.bytes())
    return pages


def licence_pages(source):
    # Complete, unabridged licence; preserve lines and indentation.
    lines = []
    for original in source.read_text(encoding="utf-8").splitlines():
        if not original:
            lines.append("")
        else:
            while len(original) > 105:
                lines.append(original[:105])
                original = original[105:]
            lines.append(original)
    output = []
    for index in range(0, len(lines), 66):
        number = index // 66 + 1
        commands = [
            "0.060 0.115 0.200 rg 0 750 595 92 re f",
            "0.020 0.520 0.560 rg 45 747 90 4 re f",
            "BT /F2 10 Tf 0.570 0.820 0.870 rg 1 0 0 1 45 803 Tm (DEFRAGMENTER / LICENCE) Tj ET",
            "BT /F2 19 Tf 1 1 1 rg 1 0 0 1 45 774 Tm (GNU General Public License) Tj ET",
            "0.810 0.850 0.880 RG 1 w 45 50 m 550 50 l S",
            f"BT /F1 9 Tf 0.320 0.390 0.450 rg 1 0 0 1 45 37 Tm (GPL v3 / full text / appendix {number:02d}) Tj ET",
        ]
        for position, line in enumerate(lines[index:index + 66]):
            line = esc(line)
            line.encode("latin-1")
            commands.append(
                f"BT /F3 7.2 Tf 0.060 0.115 0.200 rg "
                f"1 0 0 1 45 {724 - position * 10} Tm ({line}) Tj ET"
            )
        output.append(("\n".join(commands) + "\n").encode("latin-1"))
    return output


def build(source, destination, version):
    if not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+-[0-9]+", version):
        raise ValueError("Invalid source version")
    pages = guide(version) + licence_pages(source)
    objects = [b"<< /Type /Catalog /Pages 2 0 R >>", b""]
    for font in (b"Helvetica", b"Helvetica-Bold", b"Courier"):
        objects.append(b"<< /Type /Font /Subtype /Type1 /BaseFont /" + font + b" >>")
    kids = []
    for page in pages:
        kids.append(f"{len(objects) + 1} 0 R".encode("ascii"))
        objects.append(
            b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 595 842] "
            b"/Resources << /Font << /F1 3 0 R /F2 4 0 R /F3 5 0 R >> >> "
            + f"/Contents {len(objects) + 2} 0 R >>".encode("ascii")
        )
        objects.append(
            f"<< /Length {len(page)} >>\nstream\n".encode("ascii")
            + page + b"endstream"
        )
    objects[1] = (
        b"<< /Type /Pages /Kids [" + b" ".join(kids)
        + f"] /Count {len(kids)} >>".encode("ascii")
    )
    pdf = bytearray(b"%PDF-1.4\n%\xe2\xe3\xcf\xd3\n")
    offsets = []
    for index, obj in enumerate(objects, 1):
        offsets.append(len(pdf))
        pdf.extend(f"{index} 0 obj\n".encode("ascii") + obj + b"\nendobj\n")
    xref = len(pdf)
    pdf.extend(f"xref\n0 {len(objects) + 1}\n0000000000 65535 f \n".encode("ascii"))
    for offset in offsets:
        pdf.extend(f"{offset:010d} 00000 n \n".encode("ascii"))
    pdf.extend(
        f"trailer\n<< /Size {len(objects) + 1} /Root 1 0 R >>\n"
        f"startxref\n{xref}\n%%EOF\n".encode("ascii")
    )
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_bytes(pdf)


if __name__ == "__main__":
    if len(sys.argv) != 4:
        raise SystemExit("Usage: generate-manual.py LICENSE OUTPUT VERSION")
    build(Path(sys.argv[1]), Path(sys.argv[2]), sys.argv[3])
