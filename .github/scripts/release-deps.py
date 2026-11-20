"""Resolve the rolling releases once per workflow and install matching binary packages."""
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tarfile
import tempfile
from urllib.request import urlopen
import zipfile

REPOS = {'rbc': 'georgtree/rbc-tk9', 'argparse': 'georgtree/argparse'}


def api(path):
    return json.loads(subprocess.check_output(['gh', 'api', path]))


def resolve():
    tools = ('linux-gcc', 'windows-ucrt64', 'windows-msvc')
    matrix = {tool: {} for tool in tools}
    for dep, repo in REPOS.items():
        base = f'repos/{repo}'
        commit = api(f'{base}/git/ref/tags/latest')['object']['sha']
        release = api(f'{base}/releases/tags/latest')
        for tool in tools:
            suffix = f'-{tool}' + ('-tcl9.1.0-native' if dep == 'rbc' else '')
            suffix += '.tar.gz' if tool == 'linux-gcc' else '.zip'
            asset, = (a for a in release['assets'] if a['name'].startswith(dep)
                      and a['name'].endswith(suffix))
            digest = asset['digest'].removeprefix('sha256:')
            if not re.fullmatch('[0-9a-f]{64}', digest):
                raise ValueError(f'Missing SHA-256 for {asset["name"]}')
            matrix[tool][dep] = dict(name=asset['name'], url=asset['browser_download_url'],
                                     digest=digest, key=f'{commit}-{asset["id"]}-{digest}')
        if api(f'{base}/git/ref/tags/latest')['object']['sha'] != commit:
            raise RuntimeError(f'{repo} latest moved during resolution; retry the workflow')
    with open(os.environ['GITHUB_OUTPUT'], 'a', encoding='utf-8') as f:
        print('packages=' + json.dumps(matrix, separators=(',', ':')), file=f)


def install(dep, asset):
    dest = Path('_ci', dep)
    with tempfile.TemporaryDirectory() as tmp:
        archive = Path(tmp, asset['name'])
        with urlopen(asset['url'], timeout=120) as response:
            archive.write_bytes(response.read())
        if hashlib.sha256(archive.read_bytes()).hexdigest() != asset['digest']:
            raise ValueError(f'{archive.name} differs from the resolved release')
        if archive.suffix == '.zip':
            with zipfile.ZipFile(archive) as z:
                z.extractall(dest)
        else:
            with tarfile.open(archive) as t:
                t.extractall(dest, filter='data')
    # The RBC distribution also ships argparse. Expose only our chosen RBC
    # package directory to Tcl, so even an older standalone argparse wins.
    package, = (dest / 'lib').glob(dep + '[0-9]*')
    (dest / 'package-dir.txt').write_text(package.name, encoding='utf-8')


if __name__ == '__main__':
    if sys.argv[1] == 'resolve':
        resolve()
    else:
        dep = sys.argv[1]
        install(dep, json.loads(os.environ['PACKAGES'])[dep])
