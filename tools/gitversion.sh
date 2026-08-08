#!/bin/bash

shorten_name () {
  name=$1
  # issue branch?
  if [[ $name =~ ^([0-9]+)-(.+)$ ]]; then
    name="${BASH_REMATCH[1]}${BASH_REMATCH[2]}"
  fi
  # development + extra?
  if [[ $name =~ ^development-(.+)$ ]]; then
    name="dev${BASH_REMATCH[1]}"
  fi
  # release + version?
  if [[ $name =~ ^release-([0-9]\.[0-9]\.[0-9])$ ]]; then
    name="r${BASH_REMATCH[1]}"
  fi
  # cut after 6 chars
  echo ${name:0:6}
}

# The remote branch containing HEAD, so a pushed commit is named by the branch
# it lives on rather than by whatever is checked out locally.
branch=`git branch --remote --verbose --no-abbrev --contains | sed -rne 's/^[^\/]*\/([^\ ]+).*$/\1/p'`
branch="${branch#HEAD}"
if [[ -z ${branch} || "${branch}" =~ ^HEAD ]]; then
  # Nothing on a remote contains it yet, so fall back to the local name.
  branch=`git rev-parse --abbrev-ref HEAD`
fi
branch2=$(shorten_name $branch)
# --exclude='*' so local checkpoint tags do not crowd out the date, branch
# and hash: format_util_version() displays only the rightmost 25 characters.
version=`git describe --always --abbrev=7 --exclude='*' | sed -e 's/(//g' -e 's/)//g' -e's/ /_/g'`

datetime=`date +%Y%m%d.%H`
stringout="${datetime}-${branch2}-${version}"
echo $stringout

