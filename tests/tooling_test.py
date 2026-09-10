"""Validate launcher wiring and safe script entry points without installing anything."""
import json
from pathlib import Path
import subprocess
import sys

root=Path(sys.argv[1])
launch=json.loads((root/'.vscode/launch.json').read_text())
tasks=json.loads((root/'.vscode/tasks.json').read_text())
labels={task['label'] for task in tasks['tasks']}
for profile in launch['configurations']:
    assert profile['preLaunchTask'] in labels
    assert profile['type']=='cppdbg' and profile['MIMode']=='gdb'
    assert profile['externalConsole'] is False, 'ncurses needs the integrated terminal'
assert launch['configurations'][0]['args']==['--port','8787']
assert launch['configurations'][1]['args'][:2]==['--port','8788']
assert '/build/debug/daemon-config.json' in launch['configurations'][1]['args'][3]
for name in ['build-host.sh','install-daemon.sh']:
    script=root/'scripts'/name
    subprocess.run(['bash','-n',str(script)],check=True)
    result=subprocess.run(['bash',str(script),'--help'],capture_output=True,text=True,check=True)
    assert 'Usage:' in result.stdout
    if name=='install-daemon.sh':
        assert 'supports temperature forwarding for saved GPU mappings' in result.stdout
        source=script.read_text().lower()
        assert 'not implemented yet' not in source
        assert 'not temperature forwarding' not in source
        # Never run a valid installation as root from this test.
        import os
        if os.geteuid()!=0:
            denied=subprocess.run(['bash',str(script),'release','--start'],capture_output=True,text=True)
            assert denied.returncode==1
            assert 'Installation requires root' in denied.stderr
    result=subprocess.run(['bash',str(script),'--invalid'],capture_output=True)
    assert result.returncode==2
for arguments in [['--jobs'],['--jobs','0'],['--jobs','bad']]:
    result=subprocess.run(['bash',str(root/'scripts/build-host.sh'),*arguments],capture_output=True)
    assert result.returncode==2
print('VS Code launch/task wiring, shell syntax, help and invalid argument checks passed')
