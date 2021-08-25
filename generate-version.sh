#!/bin/sh

set -e

default_version=${1}
src_dir=${2}
out_file=${3}

# echo "default version:  ${default_version}"
# echo "source directory: ${src_dir}"
# echo "output file:      ${out_file}"

if [ -d "${src_dir}/.git" ] && command -v git > /dev/null; then
    workdir=$(pwd)
    cd "${src_dir}"
    git_version=$(git describe --always --tags)
    git_branch=$(git rev-parse --abbrev-ref HEAD)
    cd "${workdir}"

    new_version="${git_version} ($(date "+%b %d %Y"), branch '${git_branch}')"
else
    new_version="${default_version}"
fi

new_version="#define YAMBAR_VERSION \"${new_version}\""

if [ -f "${out_file}" ]; then
    old_version=$(cat "${out_file}")
else
    old_version=""
fi

# echo "old version: ${old_version}"
# echo "new version: ${new_version}"

if [ "${old_version}" != "${new_version}" ]; then
    echo "${new_version}" > "${out_file}"
fi
