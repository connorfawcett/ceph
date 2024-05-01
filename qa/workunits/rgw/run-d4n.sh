#!/usr/bin/env bash
set -ex
mydir=`dirname $0`

python3 -m venv $mydir
source $mydir/bin/activate
pip install pip --upgrade
pip install redis
pip install configobj
pip install boto3

# run test
$mydir/bin/python3 $mydir/test_rgw_d4n.py

deactivate

ceph_test_rgw_d4n_directory
ceph_test_rgw_d4n_policy
ceph_test_rgw_redis_driver
ceph_test_rgw_ssd_driver

echo OK.
