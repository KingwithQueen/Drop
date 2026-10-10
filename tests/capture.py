"""Force paint and inspect only a newly-created isolated production window."""
import argparse
import pathlib
import uuid
from gui import Gui
ROOT=pathlib.Path(__file__).resolve().parents[1]
def main():
    parser=argparse.ArgumentParser(); parser.add_argument('binary',type=pathlib.Path); args=parser.parse_args()
    root=ROOT/f'build/tests/visual-production-{uuid.uuid4().hex}'; root.mkdir(parents=True)
    ui=Gui(args.binary.resolve()/'drop.exe',root/'client')
    try: ui.capture(root/'production-window.png'); print(f'PASS actual painted production window: {root / "production-window.png"}',flush=True)
    finally: ui.click(126); ui.process.wait(timeout=10)
if __name__=='__main__': main()
