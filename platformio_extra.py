Import("env")
import shutil
import os

env.Replace(PROGNAME="plane-tracking")

def post_build(target, source, env):
    build_dir = env.subst("$BUILD_DIR")
    bin_file = os.path.join(build_dir, "plane-tracking.bin")
    elf_file = os.path.join(build_dir, "plane-tracking.elf")
    
    fw_bin = os.path.join(build_dir, "firmware.bin")
    fw_elf = os.path.join(build_dir, "firmware.elf")
    
    if os.path.exists(bin_file):
        shutil.copyfile(bin_file, fw_bin)
    if os.path.exists(elf_file):
        shutil.copyfile(elf_file, fw_elf)

env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", post_build)