"""
Use this task to check that parity shards in an EC pool
match the output produced by the Ceph Erasure Code Tool.
"""

import logging
import re
import json
import os
import atexit
import shutil
from io import StringIO
from io import BytesIO
from tasks import ceph_manager
from teuthology import misc as teuthology

log = logging.getLogger(__name__)

ERASURE_CODE_DIR = '/tmp/erasure-code/'
DATA_SHARD_FILENAME = 'ec-obj'


class ErasureCodeObject:
    """
    Store data relating to an RBD erasure code object,
    including the object's erasure code profile as well as
    the data for k + m shards.
    """
    def __init__(self, object_id: str, ec_profile: dict, object_size: int):
        self.object_id = object_id
        self.ec_profile = ec_profile
        self.object_size = object_size
        self.k = int(ec_profile["k"])
        self.m = int(ec_profile["m"])
        self.shards = [None] * (self.k + self.m)

    def get_ec_tool_profile(self) -> str:
        """
        Return the erasure code profile associated with the object
        in string format suitable to be fed into the erasure code tool
        """
        profile_str = ''
        for key, value in self.ec_profile.items():
            profile_str += str(key) + '=' + str(value) + ','
        return profile_str[:-1]

    def get_want_to_encode_str(self) -> str:
        """
        Return a comma seperated string of the shards
        to be produced by the EC tool encode
        This includes k + m shards as tool also produces the data shards
        """
        nums = "0,"
        for i in range(1, self.k + self.m):
            nums += (str(i) + ",")
        return nums[:-1]

    def get_object_size(self) -> int:
        """
        Return object size for the shard
        """
        return self.object_size

    def get_object_id(self) -> str:
        """
        Return the object's string identifier
        this is a combination of its oid and snapid
        """
        return self.object_id

    def get_shard(self, index: int) -> bytearray:
        """
        Return the shard (bytearray) at the specified index
        """
        return self.shards[index]

    def update_shard(self, index, data: bytearray):
        """
        Update a shard at the specified index
        """
        self.shards[index] = data

    def get_data_shards(self) -> list[bytearray]:
        """
        Return an ordered list of data shards.
        """
        return self.shards[:self.k]

    def get_parity_shards(self) -> list[bytearray]:
        """Return an ordered list of parity shards."""
        return self.shards[self.k:self.m]

    def write_data_shards_to_file(self, filepath: str):
        """
        Write the data shards to files for
        consumption by Erasure Code tool.
        """
        shards = self.get_data_shards()
        assert len(shards) == self.k
        data_out = bytearray()
        for shard in shards:
            data_out += shard
        with open(filepath, "wb") as binary_file:
            binary_file.write(data_out)

    def does_shard_match_file(self, index: int, file_in: str) -> bool:
        """
        Compare shard at specified index with contents of the supplied file
        Return True if they match, False otherwise
        """
        shard_data = self.get_shard(index)
        file_content = bytearray()
        with open(file_in, "rb") as binary_file:
            b = binary_file.read()
            file_content.extend(b)
        return shard_data == file_content

    def compare_parity_shards_to_files(self, filepath: str):
        """
        Check the object's parity shards match the files generated
        by the erasure code tool
        """
        for i in range(self.k, self.k + self.m):
            shard_filename = filepath + '.' + str(i)
            match = self.does_shard_match_file(i, shard_filename)
            if match:
                log.info("Shard %i in object %s matches file content",
                         i,
                         self.object_id)
            else:
                err = f"Shard {i} mismatch in object {self.object_id}"
                raise RuntimeError(err)


class ErasureCodeObjects:
    """
    Class for managing objects of type ErasureCodeObject
    """
    def __init__(self, manager: ceph_manager.CephManager):
        self.manager = manager
        self.os_tool = ObjectStoreTool(manager)
        self.objects = []

    def get_object_by_id(self, object_id: str) -> ErasureCodeObject:
        """
        Return the ErasureCodeObject corresponding to the supplied
        ID if it exists
        """
        for obj in self.objects:
            if obj.get_object_id() == object_id:
                return obj
        return None

    def create_ec_object(self, object_id: str,
                         ec_profile: dict, object_size: int):
        """
        Create a new ErasureCodeObject and add it to the list
        """
        ec_object = ErasureCodeObject(object_id, ec_profile, object_size)
        self.objects.append(ec_object)

    def update_object_shard(self, object_id: str,
                            shard_id: int, data: bytearray):
        """
        Update a shard of an existing ErasureCodeObject
        """
        ec_object = self.get_object_by_id(object_id)
        ec_object.update_shard(shard_id, data)

    def create_object_uid(self, info_dump: dict):
        """
        Create a unique ID for this object
        a combination of the oid and snapid
        """
        # print(info_dump["id"]["oid"])
        # print(info_dump["id"]["snapid"])
        return info_dump["id"]["oid"] + "_" + str(info_dump["id"]["snapid"])

    def process_ec_object(self, json_str: str, osd: dict):
        """
        Create a new EC object and add it to the collection,
        or update an existing one if one with the same ID is found
        """
        shard_info_dump = self.os_tool.get_shard_info_dump(osd, json_str)
        shard_id = shard_info_dump["id"]["shard_id"]
        object_id = self.create_object_uid(shard_info_dump)
        shard_data = self.os_tool.get_shard_bytes(osd, json_str)
        if self.get_object_by_id(object_id):
            self.update_object_shard(object_id, shard_id, shard_data)
        else:
            # No existing EC object, need info to create a new object
            pools_json = self.manager.get_osd_dump_json()["pools"]
            shard_pool_id = shard_info_dump["id"]["pool"]
            object_size = shard_info_dump["hinfo"]["total_chunk_size"]
            print(object_size)
            for pool_json in pools_json:
                if shard_pool_id == pool_json["pool"]:
                    ec_profile_name = self.manager.get_pool_property(
                        pool_json["pool_name"], "erasure_code_profile")
                    ec_profile_json = self.manager.raw_cluster_cmd(
                        "osd",
                        "erasure-code-profile",
                        "get",
                        ec_profile_name,
                        "--format=json"
                    )
                    break
            try:
                ec_profile = json.loads(ec_profile_json)
            except ValueError as e:
                log.error("Failed to parse object dump to JSON: %s", e)
            self.create_ec_object(object_id, ec_profile, object_size)
            self.update_object_shard(object_id, shard_id, shard_data)


class ObjectStoreTool:
    """
    Interface for running the Object Store Tool, contains functions
    for retreiving information and data from OSDs
    """
    def __init__(self, manager: ceph_manager.CephManager):
        self.manager = manager
        self.fspath = self.manager.get_filepath()

    def run_objectstore_tool(self, osd: dict, cmd: list,
                             string_out: bool = True):
        """
        Run the ceph objectstore tool.
        Execute the objectstore tool with the supplied arguments
        in cmd on the machine where the specified OSD lives.
        """
        remote = self.manager.find_remote("osd", osd)
        osd_id = osd["osd"]
        data_path = self.fspath.format(id=osd_id)
        if self.manager.cephadm:
            return shell(
                self.manager.ctx,
                self.manager.cluster,
                remote,
                args=[
                    "ceph-objectstore-tool",
                    "--err-to-stderr",
                    "--no-mon-config",
                    "--data-path",
                    data_path
                ]
                + cmd,
                name=osd,
                wait=True,
                check_status=False,
                stdout=StringIO() if string_out else BytesIO(),
                stderr=StringIO()
            )
        elif self.manager.rook:
            assert False, "not implemented"
        else:
            return remote.run(
                args=[
                    "sudo",
                    "adjust-ulimits",
                    "ceph-objectstore-tool",
                    "--err-to-stderr",
                    "--no-mon-config",
                    "--data-path",
                    data_path
                ]
                + cmd,
                wait=True,
                check_status=False,
                stdout=StringIO() if string_out else BytesIO(),
                stderr=StringIO()
            )

    # def get_rbd_data_objects(self, osd: dict) -> list[str]:
    #     """
    #     Return list of string IDs of RBD objects living on this OSD.
    #     """
    #     object_names = []
    #     proc = self.run_objectstore_tool(osd, ["--op", "list"])
    #     stdout = proc.stdout.getvalue()
    #     if not stdout:
    #         log.error("Objectstore tool failed with error "
    #                   "when retreiving list of data objects")
    #     else:
    #         object_names = re.findall('(rbd_data.*?)"', stdout)
    #     return object_names

    def get_ec_data_objects(self, osd: dict) -> list[dict]:
        """
        Return list of erasure code objects living on this OSD.
        """
        objects = []
        proc = self.run_objectstore_tool(osd, ["--op", "list"])
        stdout = proc.stdout.getvalue()
        if not stdout:
            log.error("Objectstore tool failed with error "
                      "when retreiving list of data objects")
        else:
            for line in stdout.split('\n'):
                if line:
                    try:
                        shard = json.loads(line)
                        # print(shard)
                        #TODO Is there a better/more consistent way
                        # to identify an EC shard?
                        if 's' in shard[0] and shard[1]["oid"]:
                            # print(shard[0])
                            objects.append(shard)
                    except ValueError as e:
                        log.error("Failed to parse shard list to JSON: %s", e)

        return objects

    def get_shard_info_dump(self, osd: dict, json_str: str) -> dict:
        """
        Return the JSON formatted shard information living on specified OSD.
        json_str is the line of the string produced by the OS tool 'list'
        command which corresponds to a given shard
        """
        shard_info = {}
        proc = self.run_objectstore_tool(osd, ["--json", json_str, "dump"])
        stdout = proc.stdout.getvalue()
        if not stdout:
            log.error("Objectstore tool failed with error "
                      "when dumping object info.")
        else:
            try:
                shard_info = json.loads(stdout)
            except ValueError as e:
                log.error("Failed to parse object dump to JSON: %s", e)
        return shard_info

    def get_shard_bytes(self, osd: dict, object_id: str) -> bytearray:
        """
        Return the contents of the shard living on the specified OSD as bytes.
        """
        proc = self.run_objectstore_tool(osd, [object_id, "get-bytes"], False)
        stdout = proc.stdout.getvalue()
        if not stdout:
            log.error("Objectstore tool failed to get shard bytes.")
        else:
            return bytearray(stdout)


class ErasureCodeTool:
    """
    Interface for running the Ceph Erasure Code Tool
    """
    def __init__(self, manager: ceph_manager.CephManager, osd: dict):
        self.manager = manager
        self.remote = self.manager.find_remote("osd", osd)

    def run_erasure_code_tool(self, cmd: list[str]):
        """
        Run the ceph erasure code tool with the arguments in the supplied list
        """
        args = ["sudo", "adjust-ulimits", "ceph-erasure-code-tool"] + cmd
        if self.manager.cephadm:
            return shell(
                self.manager.ctx,
                self.manager.cluster,
                self.remote,
                args=args,
                name=None,
                wait=True,
                check_status=False,
                stdout=StringIO(),
                stderr=StringIO(),
            )
        elif self.manager.rook:
            assert False, "not implemented"
        else:
            return self.remote.run(
                args=args,
                wait=True,
                check_status=False,
                stdout=StringIO(),
                stderr=StringIO(),
            )

    def calc_chunk_size(self, profile: str, object_size: str) -> int:
        """
        Returns the chunk size for the given 
        """
        cmd = ["calc-chunk-size", profile, object_size]
        proc = self.run_erasure_code_tool(cmd)
        stdout = proc.stdout.getvalue()
        if not stdout:
            log.error("Erasure Code tool failed to calculate chunk size.")
        return stdout

    def encode(self, profile: str, stripe_unit: int,
               file_nums: str, filepath: str):
        """
        Encode the specified file using the erasure code tool
        Output will be written to files in the same directory
        """
        cmd = ["encode", profile, str(stripe_unit), file_nums, filepath]
        proc = self.run_erasure_code_tool(cmd)
        if proc.exitstatus != 0:
            log.error("Erasure Code tool failed to encode.")

    def decode(self, profile: str, stripe_unit: int,
               file_nums: str, filepath: str):
        """
        Decode the specified file using the erasure code tool
        Output will be written to files in the same directory
        """
        cmd = ["decode", profile, str(stripe_unit), file_nums, filepath]
        proc = self.run_erasure_code_tool(cmd)
        if proc.exitstatus != 0:
            log.error("Erasure Code tool failed to decode.")


def shell(ctx: any, cluster_name: str, remote: any,
          args: list, name: str = None, **kwargs: any):
    """
    Interface for running commands on cephadm clusters
    """
    extra_args = []
    if name:
        extra_args = ['-n', name]
    return remote.run(
        args=[
            'sudo',
            ctx.cephadm,
            '--image', ctx.ceph[cluster_name].image,
            'shell',
        ] + extra_args + [
            '--fsid', ctx.ceph[cluster_name].fsid,
            '--',
        ] + args,
        **kwargs
    )


def exit_handler(manager: ceph_manager.CephManager, osds: list):
    """
    Revive any OSDs that were killed during the task and
    clean up any temporary files
    """
    for osd in osds:
        osd_id = osd["osd"]
        manager.revive_osd(osd_id, skip_admin_check=True)  # TODO remove skip
    # shutil.rmtree(ERASURE_CODE_DIR)


def task(ctx, config: dict):
    """
    Gather all EC object shards, run the data shards through the EC tool
    and verify the encoded output matches the parity shards on the OSDs
    """
    if config is None:
        config = {}

    first_mon = teuthology.get_first_mon(ctx, config)
    (mon,) = ctx.cluster.only(first_mon).remotes.keys()

    manager = ceph_manager.CephManager(
        mon,
        ctx=ctx,
        logger=log.getChild("ceph_manager"),
    )

    osds = manager.get_osd_dump()
    os_tool = ObjectStoreTool(manager)
    ec_tool = ErasureCodeTool(manager, osds[0])
    ec_objects = ErasureCodeObjects(manager)
    atexit.register(exit_handler, manager, osds)

    # Loop through every OSD, storing each object shard in an EC object
    for osd in osds:
        osd_id = osd["osd"]
        manager.kill_osd(osd_id)
        data_objects = os_tool.get_ec_data_objects(osd)
        if data_objects:
            for data_object in data_objects:
                json_str = json.dumps(data_object)
                ec_objects.process_ec_object(json_str, osd)
        else:
            pass #TODO

    # Now compute the parities for each object
    # and verify they match what the EC tool produces
    for ec_object in ec_objects.objects:
        # Create dir and write out shards
        object_id = ec_object.get_object_id()
        object_dir = ERASURE_CODE_DIR + object_id + '/'
        object_filepath = object_dir + DATA_SHARD_FILENAME
        os.makedirs(object_dir)
        ec_object.write_data_shards_to_file(object_filepath)

        # Encode the shards and output to the object dir
        want_to_encode = ec_object.get_want_to_encode_str()
        ec_profile = ec_object.get_ec_tool_profile()
        object_size = ec_object.get_object_size()
        ec_tool.encode(ec_profile,
                       object_size,
                       want_to_encode,
                       object_filepath)

        # Finally compare stored parities to EC tool output
        ec_object.compare_parity_shards_to_files(object_filepath)
