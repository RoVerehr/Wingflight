"""Fetch the exact dependency sources after downloading GitHub's source ZIP."""
from pathlib import Path
import json,subprocess
ROOT=Path(__file__).resolve().parents[1]
def git(*args):return subprocess.run(['git',*map(str,args)],check=True,capture_output=True,text=True).stdout.strip()
def main():
    deps=json.loads((ROOT/'dependencies.json').read_text(encoding='utf-8'))['dependencies']
    for d in deps:
        target=ROOT/d['path']
        if not target.exists():
            target.parent.mkdir(parents=True,exist_ok=True)
            git('clone','--no-checkout',d['repository'],target)
            git('-C',target,'checkout','--detach',d['commit'])
        head=git('-C',target,'rev-parse','HEAD')
        if head!=d['commit']:raise RuntimeError(str(target)+' is at a different commit; existing checkout was not changed.')
        if d['path']=='vendor/CommonLibSSE-NG':git('-C',target,'submodule','update','--init','--recursive')
        print(d['path']+' '+head)
if __name__=='__main__':main()
