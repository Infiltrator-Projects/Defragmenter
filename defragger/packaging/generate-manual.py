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
import textwrap


INK = (.060, .115, .200)
MUTED = (.320, .390, .450)
TEAL = (.020, .520, .560)
PALE = (.930, .960, .970)


def colour(rgb):
    return " ".join(str(n) for n in rgb)


def esc(text):
    return text.replace("\\", "\\\\").replace("(", "\\(").replace(")", "\\)")


class Page:
    def __init__(self, title, version, number, total):
        self.parts = []
        self.rect(0, 750, 595, 92, INK)
        self.rect(45, 747, 90, 4, TEAL)
        self.text(45, 803, "INFILTRATOR  /  APPLICATIONS", 10, "F2", (.570, .820, .870))
        self.text(45, 774, title, 20, "F2", (1, 1, 1))
        self.parts.append("0.810 0.850 0.880 RG 1 w 45 50 m 550 50 l S")
        self.text(45, 37, f"Defragmenter  /  {version}  /  User manual", 9, "F1", MUTED)
        self.text(514, 37, f"{number:02d} / {total:02d}", 9, "F1", MUTED)

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
    # All user documentation is authored once here and built deterministically.
    catalogue = [["Overview and safety",[["Purpose",["Analyse/Map inspects filesystem allocation and fragmentation without modifying a target. Defragment and Growth Defrag move supported allocations on clean, unmounted volumes. Recover uses verified transaction state after interruption.","This is an offline maintenance application, not an online defragmenter, general repair suite or replacement kernel filesystem driver."]],["Supported does not mean universal",["Each write is limited to explicitly qualified format features and geometry. Missing modules, mounted targets, uncertain metadata and stale device identity cause pre-write refusal.","Synthetic tests, a passing CI run, a published package and a successful physical-media test are different claims. Do not substitute one for another."]],["Destructive test media",["The separate Test Media program intentionally erases sacrificial disks. Never use it on a system disk or on any device with irreplaceable data."]]]],["Choosing a safe target",[["Selection",["Choose the intended unmounted volume or supported image in Defragmenter. Check device, capacity, filesystem type and operation before confirming.","Labels and desktop discovery hints help select a candidate but do not authorise mutation."]],["Mandatory write admission",["The worker binds device identity and capacity, refuses mounted or overlapping devices, rechecks the target at privilege boundaries and rejects unexpected hardware or geometry changes.","The selected external filesystem .so must be trusted, ABI-compatible and capable of validating the selected operation."]],["When refused",["A busy target, unknown format feature, unsupported geometry or missing module is a refusal, not permission for an automatic fallback or risky retry.","Recover differs: an interrupted volume may be unreadable to fresh analysis; a valid existing target-bound journal can still authorise its own recovery path."]]]],["Analyse and allocation map",[["Read-only analysis",["Analyse enumerates physical allocation, file fragmentation and reserved or unknown ranges only within the validated reader subset.","Incomplete accounting must be displayed as uncertain rather than silently treated as empty or presenting a false exact percentage."]],["Reading the map",["Physical order runs from left to right, then top to bottom. Adjacent pixels may magnify a known unit or represent neighbouring physical ranges combined in order.","The diagram is schematic; it does not depict actual sectors from the selected device."]],["Interpretation",["A successful Analyse operation is not approval to write. Defragment and Growth each need additional module, layout, target and recoverability checks."]]]],["Defragment operation",[["Objective",["Move independently movable regular-file allocations into a verified canonical arrangement. This does not repair arbitrary corruption or guarantee one extent per file for every supported format.","The generic planner proposes placement. The authoritative filesystem module must validate and translate the plan into safe metadata and transaction operations."]],["Before writing",["Verify selected identity, unmounted state, supported features and operation capabilities. Required staging and durable recovery records precede authoritative changes.","Failed preflight must not clear a volume signature or partially prepare the device."]],["Successful completion",["The worker must reopen the result and independently verify all retained logical file bytes, relevant metadata invariants, non-overlap and the required physical placement.","A zero exit code, a formatted test disk or a successful read-only Verify action alone cannot establish a passed defragmentation qualification."]],["Limitations",["ZFS remains analysis-only in shipped builds while its filesystem-owned transaction-safe defragmenter is developed. Swap has no regular files and therefore no regular-file defragmentation contract."]]]],["Growth Defrag operation",[["Exact reserve in allocation units",["Growth requests 10 percent free space immediately after each regular file, rounded UP to whole filesystem allocation units. A 19-unit file requires 2 adjacent free units; a 1-unit file requires 1.","For a split file, the planned reserve follows the final extent inside its provider-certified free range."]],["Constraints",["The module certifies the allocation-unit size, alignment, pinned/shared ranges and real layout restrictions. Alignment padding can introduce extra space; fixed metadata can make the requested plan impossible.","An impossible or unsupported result is refused rather than silently reducing the reserve."]],["Planner status",["Defragmenter's pure C planner handles a certified contiguous region or proposes extents across sorted disjoint certified free runs, preferring a single fit.","Neither planner writes a disk. General inventory, plan execution, transaction translation and cross-filesystem production migration are incomplete."]]]],["Stop and Recover",[["Stop",["Stop is cooperative. A worker may need to reach a valid or recoverable transaction boundary before exiting.","A closed GUI, interrupted protocol or worker termination is not evidence that on-disk recovery requirements have been satisfied."]],["Durable recovery record",["A journal binds target identity, capacity, transaction phase and verified staged bytes. Records are validated before recovery is admitted.","Recover must not invent an unknown transaction or accept mismatched targets, missing data, tampered staged hashes or invalid phases."]],["After an interrupted operation",["Preserve recovery files and report the exact failed phase. Do not erase or reformat a partly modified target just to remove a warning.","Recovery-state cleanup occurs only after verified completion. Process-kill and synthetic replay tests do not prove protection against every physical power interruption."]]]],["Test Media: Build",[["Choose a disposable disk",["Run /Programs/defragmenter-test-media and select the correct physical sacrificial disk. The '1. Build test disk' action destroys its existing partitions and data.","The current Test Media layout contains 21 fixed filesystem slots while full dynamic capability discovery is being developed."]],["Deterministic corpus",["The large file sizes are 4, 6, 10, 16, 26, 34, 44 and 60 MiB, totalling 200 MiB, plus 0, 1, 511, 512, 513, 4095, 4096 and 4097 byte boundary files.","Fixture creation and raw verification are first-party native C paths, not host formatting utilities or mounted target files."]],["Trust the actual bytes",["Protected schema 3 state binds the disk fingerprint, corpus version and per-slot build outcomes. Its records are not substitutes for rereading actual payload bytes.","Disks created under obsolete schema 2 require rebuilding before current qualification."]]]],["Test Media: Qualify and Verify",[["2. Qualify engines",["Requests actual Defragment and Growth Defrag on applicable partitions, then verifies retained file content. A skipped or unimplemented requested mutation is a failed mutation qualification.","An analysis-only slot cannot be counted as a successful defragmentation engine simply because its files remain readable."]],["3. Verify files",["Independently rereads the deterministic corpus and validates retained logical payload bytes. It does not establish that any file movement, Growth reserve or recovery operation happened.","Build, Qualify and Verify report separate results; inspect per-slot status and diagnostic logs rather than interpreting a green summary as proof of all operations."]],["Known unavailable mutations",["ZFS and swap are present in the transitional layout but have no qualified production Defrag or Growth writer. Their Verify action may pass without changing that capability status.","A 21/21 production Defrag qualification is not currently established."]]]],["Independent filesystem modules",[["Installed ownership",["Independent filesystem engines are installed under /System/Filesystems as individually versioned .so files. Private application workers belong instead under /System/Commands/Defragmenter.","Module identity, filesystem type, capabilities, ABI, ownership and trust path must match before it can perform an operation."]],["Bounded filesystem families",["Qualified subsets include FAT12/16/32, exFAT, NTFS, EXT2/3/4, XFS v5, Amiga OFS/FFS/SFS/PFS3, classic HFS/HFS+/HFSX, Btrfs, APFS, Minix and UFS1/UFS2.","Encryption, sharing, snapshots, dirty metadata or unrecognised feature combinations may cause write refusal even for a listed family."]],["Migration is not finished",["The native engine supports --list-installed-modules for compatible generic Defrag V2 providers. GUI and Test Media discovery are not yet fully dynamic.","OFS has a bounded shared-planner pilot. Existing safe filesystem-specific movers remain until fully equivalent, independently verified replacements exist."]]]],["Installation and releases",[["Application layout",["The generic Debian package installs /Programs/defragmenter and /Programs/defragmenter-test-media plus a single searchable user manual in /System/Resources/Documentation/Defragmenter.pdf.","The obsolete documentation subfolder is retired only when empty; unrelated user content is preserved."]],["Generic and CPU-native choices",["The published amd64 .deb is built for broad compatibility. The versioned .run contains committed source and a pinned Common revision, then builds a CPU-native Debian package.","The .run supports --build-only --output DIRECTORY for building a package without installing it."]],["Release evidence",["Only a defragger/VERSION change expresses release intent. Both mandatory quality lanes, exact source/governance audit checks and checksum-bound, once-built assets must pass.","GitHub release publication and public APT availability are verified separately; neither can be inferred from a new Git commit alone."]]]],["Diagnostics and assurance",[["Understanding failure",["A missing or incompatible .so, mounted-overlap refusal, target replacement, unsupported feature, transaction failure or verification mismatch must be treated as a real negative outcome.","Record the operation, exact application/module versions, relevant device type, logged phase and returned status, without including unrelated private information."]],["Physical-media limits",["Host-image tests, sanitizers and replay fixtures cannot reproduce every physical SD-card controller, removable-media race, firmware flush defect or unexpected whole-system power loss.","Physical /dev/mmcblk0p11 qualification requires a controlled sacrificial-device run and its own observed result. A GitHub workflow cannot certify that result."]],["Canonical engineering documents",["Maintained source design, module ownership, safety decisions, release audit identities and exact support boundaries live in the Defragmenter GitHub repository.","This manual describes current bounded operation. It does not certify completion of the proposed universal filesystem module migration."]]]]]
    output = []
    total = len(catalogue)
    for number, (title, sections) in enumerate(catalogue, 1):
        page = Page(title, version, number, total)
        y = 700
        for heading, paragraphs in sections:
            page.section(heading, y)
            y -= 33
            for paragraph in paragraphs:
                for line in textwrap.wrap(paragraph, width=86,
                                          break_long_words=False,
                                          break_on_hyphens=False):
                    page.text(53, y, line, 10.2)
                    y -= 15
                y -= 8
            y -= 13
        if number == 3:
            if y < 222:
                raise ValueError("Map illustration overlaps user manual text")
            page.text(53, 189, "SCHEMATIC: FRAGMENTED / PACKED", 10, "F2", MUTED)
            for i in range(12):
                page.rect(53 + i * 40, 152, 32, 23,
                          TEAL if i in (0, 2, 5, 8, 11) else (.72, .80, .83))
                page.rect(53 + i * 40, 113, 32, 23,
                          TEAL if i < 5 else (.72, .80, .83))
            page.text(53, 91, "Schematic only; not actual sectors.", 9, "F1", MUTED)
        elif y < 88:
            raise ValueError("User manual page overflows: " + title)
        output.append(page.bytes())
    return output


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
