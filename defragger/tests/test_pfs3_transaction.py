#!/usr/bin/python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""End-to-end bounded PFS3 transaction and recovery qualification."""
from __future__ import annotations
import json, os, stat, subprocess, tempfile
from pathlib import Path
BUILD=Path(os.environ["LINUX_DEFRAGGER_BUILD_DIR"])
FIXTURE=BUILD/"linux-defragger-pfs3-native-test"
WORKER=BUILD/"linux-defragger-pfs3-worker"
def run(*args: object)->subprocess.CompletedProcess[str]:
    return subprocess.run([str(WORKER),*map(str,args)],check=False,text=True,
        stdout=subprocess.PIPE,stderr=subprocess.PIPE,
        env={**os.environ,"LC_ALL":"C","LANG":"C"})
def make_fixture(path:Path)->None:
    subprocess.run([str(FIXTURE),"--write-fixture",str(path),"fragmented"],check=True)
def analysis(path:Path)->dict:
    c=run("analyse-json",path); assert c.returncode==0,c.stderr; return json.loads(c.stdout)
def test_defrag(root:Path)->None:
    image=root/"defrag.pfs3"; journal=root/"defrag.journal"; make_fixture(image)
    c=run("defrag",image,"--write","--confirm",image,"--journal",journal)
    assert c.returncode==0,c.stderr; p=analysis(image); assert p["format"]=="PFS3" and p["fragmented_files"]==0
    assert not journal.exists() and not Path(str(journal)+".pfs3-stage").exists()
def test_growth(root:Path)->None:
    image=root/"growth.pfs3"; journal=root/"growth.journal"; make_fixture(image)
    c=run("growth-defrag",image,"--write","--confirm",image,"--journal",journal,"--growth-percent","10")
    assert c.returncode==0,c.stderr; p=analysis(image)
    assert p["format"]=="PFS3" and p["fragmented_files"]==0 and p["growth_10_satisfied"] is True
def test_recovery(root:Path)->None:
    image=root/"recover.pfs3"; journal=root/"recover.journal"; make_fixture(image)
    image.chmod(stat.S_IRUSR)
    failed=run("defrag",image,"--write","--confirm",image,"--journal",journal)
    image.chmod(stat.S_IRUSR|stat.S_IWUSR); assert failed.returncode!=0
    stage=Path(str(journal)+".pfs3-stage"); assert journal.exists() and stage.exists()
    recovered=run("recover",image,"--write","--confirm",image,"--journal",journal)
    assert recovered.returncode==0,recovered.stderr; assert analysis(image)["fragmented_files"]==0
    assert not journal.exists() and not stage.exists()
def main()->int:
    with tempfile.TemporaryDirectory(prefix="linux-defragger-pfs3-transaction-") as temp:
        root=Path(temp); test_defrag(root); test_growth(root); test_recovery(root)
    return 0
if __name__=="__main__": raise SystemExit(main())
