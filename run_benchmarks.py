#!/usr/bin/python3

import subprocess
import sys
import ipdb
import os

# we want to check whether the current git repo is dirty
# we want to record the command line parameters

status=subprocess.run('git diff-files --quiet'.split(" "))
status=status.returncode

if status != 0:
    print("git reports uncommitted code. Commit and try again")
    exit(status)


if len(sys.argv) != 2:
    print("You must name command line parameter for test program")
    exit(1)

working_directory = os.path.dirname(sys.argv[0])
script_directory = working_directory

# create build directory 

directory_path = '{}/build_benchmark/'.format(working_directory) # specify path where you want to create the directory
if not os.path.exists(directory_path): # check if the specified path exists
    try:
        os.makedirs(directory_path) # create the new directory if it doesn't exist
        print('Directory created successfully!')
    except OSError as e:
        print('Error creating directory:', str(e))

measurements_dir = directory_path + "/perf"

if not os.path.exists(measurements_dir): # check if the specified path exists
    try:
        os.makedirs(measurements_dir) # create the new directory if it doesn't exist
        print('Directory created successfully!')
    except OSError as e:
        print('Error creating directory:', str(e))

working_directory=directory_path

p = subprocess.run("cmake -G Ninja ..".split(" "), capture_output=True, cwd=working_directory)
p = subprocess.run("ninja".split(" "), capture_output=True, cwd=working_directory)


invocation_param = sys.argv[1]

status=subprocess.run('ninja', cwd=working_directory)
status=status.returncode


if status != 0:
    print("ninja build did return status other than 0: {}".format(status))
    exit(status)


p=subprocess.run('git show --format="%h" --no-patch'.split(" "), capture_output=True)
git_hash = p.stdout.split(b'"')[1].decode('utf-8')

# for i in range(1,33):
#     print("Starting run {0}".format(i))
#     p = subprocess.run(("sudo /usr/bin/perf stat -o ./perf/perf_{0}.csv -x \\t -r 1 ./tasks {0} {1} > ./perf/out_{0}.txt".format(i, invocation_param)) .split(" "), capture_output=True, cwd=working_directory)
#     f = open("{0}/perf/out_{1}.txt".format(working_directory,str(i)), 'wb')
#     f.write(p.stdout)
#     f.close()
#     if p.returncode != 0 :
#         print("program errored out: {}".format(p.returncode))
#         exit(p.returncode)
#     print("Completed run {0}\n".format(i))

# for all .csv files in directory, fetch the filename and the first cell in the third line and add this as a line in the summary file

csv_files = [f for f in os.listdir("{}/perf/".format(working_directory)) if f.endswith(".csv")]

report_filename = "{2}/report_{0}_{1}.csv".format(invocation_param, git_hash, script_directory)

of = open(report_filename, 'w' )
of.write("# invocation param: '{}'\n".format(invocation_param))
of.write("# performance comparison at git hash: {}\n".format(git_hash))
of.write("Thread count\tWall clock time (ms)\n".format(git_hash))

for filename in csv_files:

    f = open( "{0}/perf/{1}".format(working_directory,filename), 'r')

    f.readline() # ignore line 1
    f.readline() # ignore line 2
    entry = f.readline()
    f.close()

    num_threads = filename.split(".")[0].split("_")[1]
    of.write("{}\t{}\n".format( num_threads, entry.split("\t")[0]))


print("Wrote report to file: {}\n".format(report_filename))
