# VMprof Python client

[![Build Status on TravisCI](https://travis-ci.org/vmprof/vmprof-python.svg?branch=master)](https://travis-ci.org/vmprof/vmprof-python)
[![Build Status on TeamCity](https://teamcity.jetbrains.com/app/rest/builds/buildType:(id:VMprofPython_TestsPy27Win)/statusIcon)](https://teamcity.jetbrains.com/project.html?projectId=VMprofPython)
[![Read The Docs](https://readthedocs.org/projects/vmprof/badge/?version=latest)](https://vmprof.readthedocs.org/en/latest/)


See https://vmprof.readthedocs.org for up to date info

basic usage:

sudo apt-get install python-dev libdwarf-dev libelfg0-dev libunwind8-dev

pip install vmprof

python -m vmprof <your program> <your program args>


## vmprofshow

`vmprofshow` is a command line tool that comes with **VMprof** which can read profile files generated by **VMprof** and produce a nicely formatted output.

Here is an example of how to use:

Run a smallish program which burns CPU cycles (with vmprof enabled):

```console
pypy test/cpuburn.py
```

This will produce a profile file `vmprof_cpuburn.dat`.

Now display the profile:

```console
vmprofshow vmprof_cpuburn.dat
```

You will see a (colored) output:

```console
oberstet@thinkpad-t430s:~/scm/vmprof-python$ vmprofshow vmprof_cpuburn.dat
100.0%  <module>  100.0%  tests/cpuburn.py:1
100.0% .. test  100.0%  tests/cpuburn.py:35
100.0% .... burn  100.0%  tests/cpuburn.py:26
 99.2% ...... _iterate  99.2%  tests/cpuburn.py:19
 97.7% ........ _iterate  98.5%  tests/cpuburn.py:19
 22.9% .......... _next_rand  23.5%  tests/cpuburn.py:14
 22.9% ............ JIT code  100.0%  0x7fa7dba57a10
 74.7% .......... JIT code  76.4%  0x7fa7dba57a10
  0.1% .......... JIT code  0.1%  0x7fa7dba583b0
  0.5% ........ _next_rand  0.5%  tests/cpuburn.py:14
  0.0% ........ JIT code  0.0%  0x7fa7dba583b0
```
