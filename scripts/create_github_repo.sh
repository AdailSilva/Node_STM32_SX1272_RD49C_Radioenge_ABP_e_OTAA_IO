#!/bin/sh
# Creates the GitHub repository for this project and publishes the first version.
# Run it once, from the project folder, with the GitHub CLI logged in (gh auth login):
#
#   sh scripts/create_github_repo.sh                # public repository
#   sh scripts/create_github_repo.sh --private      # private repository
#   sh scripts/create_github_repo.sh --sync         # repository already on GitHub: only
#                                                   # description, topics and tags
#   VER=0.0.2 sh scripts/create_github_repo.sh   # other first version (tag v0.0.2)
#   MSG="texto" sh scripts/create_github_repo.sh  # message of the commit with the pending changes
#   TRIES=8 WAIT=600 sh scripts/create_github_repo.sh  # attempts / seconds between them when GitHub
#                                                   # limits repository creation (default 6 x 300 s)
#
# Folder without .git      -> new local repository, first commit.
# Folder with .git already -> history kept; pending changes go into one commit and
#                             "origin" is pointed to the new repository. The tags of the
#                             local history are pushed too, so a repository deleted on
#                             GitHub and created again gets its releases back.
# The GitHub Pages site (web page of the node, web/index.html) is turned on too: the
# workflow publishes it at https://<user>.github.io/<repository>/.
set -e
cd "$(dirname "$0")/.."

REPO="Node_STM32_SX1272_RD49C_Radioenge_ABP_e_OTAA_IO"
# Previous names of the folder and of the repository (a repository with one of them is
# renamed to $REPO, see below). Empty on purpose: no other repository is ever renamed
# into this one.
OLD_REPOS=""
DESC="LoRaWAN node for the Radioenge module (STM32 + SX1272, AU915) with relays and PZEM-004T energy meters, ADAILSILVA_V1 protocol and AT commands."
# Website of the repository (field "Website" on GitHub)
HOMEPAGE="https://chirpstack.adailsilva.com.br/"
TOPICS="lorawan,lora,stm32,stm32l0,sx1272,radioenge,loramac-node,abp,otaa,chirpstack,iot,au915,flutter,web-serial,at-commands,relay,pzem-004t,modbus,energy-meter"
VISIBILITY=--public
MODE=create
for a in "$@"; do
	case "$a" in
	--public|--private|--internal) VISIBILITY="$a" ;;
	--sync) MODE=sync ;;
	*) echo "usage: $0 [--public|--private] [--sync]"; exit 1 ;;
	esac
done
VER="${VER:-0.0.1}"   # first tag; the firmware version comes from the Git tag

command -v gh >/dev/null || { echo "GitHub CLI (gh) not found"; exit 1; }
gh auth status >/dev/null 2>&1 || { echo "run 'gh auth login' first"; exit 1; }
git config user.name >/dev/null && git config user.email >/dev/null || {
	echo "git has no author: git config --global user.name \"Seu Nome\" && git config --global user.email voce@exemplo.com"
	exit 1
}
echo "$VER" | grep -Eq '^[0-9]+\.[0-9]+\.[0-9]+(-[0-9A-Za-z.]+)?$' || { echo "VER '$VER' is not MAJOR.MINOR.PATCH"; exit 1; }

GH_USER=$(gh api user -q .login)
echo "user: $GH_USER  repo: $REPO  version: v$VER"

# Renamed project: a repository on GitHub with one of the previous names becomes this one
# (gh repo rename keeps history, Releases and stars, and GitHub redirects the old URL).
# Then it goes on as --sync (description, topics, Pages and tags). The "origin" of the local
# history still pointing to a previous name is pointed to the new one.
for OLD in $OLD_REPOS; do
	if ! gh repo view "$GH_USER/$REPO" >/dev/null 2>&1 && gh repo view "$GH_USER/$OLD" >/dev/null 2>&1; then
		echo "renaming $GH_USER/$OLD -> $REPO on GitHub"
		gh repo rename "$REPO" -R "$GH_USER/$OLD" --yes || { echo "ERROR: could not rename $GH_USER/$OLD"; exit 1; }
		MODE=sync
	fi
	if [ -d .git ] && git remote get-url origin 2>/dev/null | grep -q "/$OLD\(\.git\)\?$"; then
		git remote set-url origin "https://github.com/$GH_USER/$REPO.git"
		echo "origin -> https://github.com/$GH_USER/$REPO.git"
	fi
done

# ---------------------------------------------------------------- helpers
# Topics ("tags" of the repository page) + description + website. Retried: right after
# "gh repo create" GitHub may still answer 404 for a few seconds.
set_topics() {
	for i in 1 2 3 4 5; do
		if gh repo edit "$GH_USER/$REPO" --description "$DESC" --homepage "$HOMEPAGE" --add-topic "$TOPICS" >/dev/null 2>&1; then
			echo "topics: $TOPICS"
			return 0
		fi
		sleep 3
	done
	echo "WARNING: topics not applied. Try again later with: sh scripts/create_github_repo.sh --sync"
	return 1
}

# Git tags: the ones of the local history go up as they are (each one makes GitHub
# Actions build the firmware and publish its Release with the .bin files); v$VER is
# created on the current commit when it does not exist ($1 = "always") or when the
# history has no tag at all.
push_tags() {
	if [ -n "$(git tag -l)" ]; then
		echo "tags of the local history: $(git tag -l | tr '\n' ' ')"
		git push -q origin --tags || { echo "WARNING: could not push the tags"; return 1; }
	fi
	if git rev-parse -q --verify "refs/tags/v$VER" >/dev/null; then
		if [ "$(git rev-list -n 1 "v$VER")" != "$(git rev-parse HEAD)" ]; then
			echo "tag v$VER is on an older commit: the current one has no Release yet."
			echo "Publish it with the next version, e.g.: git tag -a v0.0.2 -m v0.0.2 && git push origin v0.0.2"
		fi
	elif [ "$1" = always ] || [ -z "$(git tag -l)" ]; then
		git tag -a "v$VER" -m "v$VER"
		git push -q origin "v$VER" || { echo "WARNING: could not push v$VER"; return 1; }
		echo "tag v$VER created and pushed (GitHub Actions publishes its Release)"
	fi
}

# GitHub limits how many repositories an account can create in a short time
# ("You have created too many repositories, too quickly"). The limit is temporary:
# the script waits and tries again. If it is still there after $TRIES attempts it
# exits with code 75; the local commit is kept, so just run the same command later.
TRIES="${TRIES:-6}"
WAIT="${WAIT:-300}"
create_repo() {
	try=1
	while :; do
		if out=$(gh repo create "$REPO" "$VISIBILITY" --description "$DESC" 2>&1); then
			[ -n "$out" ] && echo "$out"
			return 0
		fi
		# sometimes GitHub answers with an error but the repository was created
		if gh repo view "$GH_USER/$REPO" >/dev/null 2>&1; then
			echo "repository $GH_USER/$REPO exists (GitHub answered: $out)"
			return 0
		fi
		echo "$out"
		case "$out" in
		*"too many repositories"*|*"too quickly"*|*"secondary rate limit"*|*"abuse detection"*)
			if [ "$try" -ge "$TRIES" ]; then
				echo "GitHub is still limiting repository creation after $TRIES attempts."
				echo "Nothing was lost (the local commit is kept). Run the same command again later."
				return 75
			fi
			echo "GitHub limit on repository creation: waiting $WAIT s before attempt $((try + 1))/$TRIES (Ctrl+C stops; running again later is safe)"
			sleep "$WAIT"
			try=$((try + 1))
			;;
		*) return 1 ;;
		esac
	done
}

# Push the current branch. Retried: right after creation GitHub may answer 404 for a few seconds.
push_branch() {
	for i in 1 2 3 4 5; do
		git push -q -u origin HEAD && return 0
		sleep 5
	done
	echo "ERROR: could not push. When GitHub answers again: git push -u origin HEAD && sh scripts/create_github_repo.sh --sync"
	return 1
}

# GitHub Pages published by the workflow (job "pages"). Never stops the script.
enable_pages() {
	if gh api "repos/$GH_USER/$REPO/pages" >/dev/null 2>&1; then
		gh api -X PUT "repos/$GH_USER/$REPO/pages" -f build_type=workflow >/dev/null 2>&1 || true
	elif ! gh api -X POST "repos/$GH_USER/$REPO/pages" -f build_type=workflow >/dev/null 2>&1; then
		echo "WARNING: GitHub Pages not turned on (private repository on a free plan?). Settings > Pages > Source: GitHub Actions"
		return 0
	fi
	echo "web page: https://$GH_USER.github.io/$REPO/ (published by the workflow)"
}

# What GitHub has now.
show_remote() {
	echo
	echo "on GitHub:"
	echo "  topics: $(gh api "repos/$GH_USER/$REPO/topics" -q '.names | join(", ")' 2>/dev/null || echo '?')"
	echo "  tags  : $(git ls-remote --tags origin 2>/dev/null | sed -n 's#.*refs/tags/\([^^]*\)$#\1#p' | tr '\n' ' ')"
	echo "https://github.com/$GH_USER/$REPO"
	echo "build/release/pages: https://github.com/$GH_USER/$REPO/actions"
}

# --------------------------------------------------- --sync (repo already on GitHub)
if [ "$MODE" = sync ]; then
	gh repo view "$GH_USER/$REPO" >/dev/null 2>&1 || { echo "$GH_USER/$REPO does not exist on GitHub: run without --sync"; exit 1; }
	[ -d .git ] || { echo "no local git history here: nothing to push (run without --sync after deleting the repository on GitHub)"; exit 1; }
	git remote get-url origin >/dev/null 2>&1 || git remote add origin "https://github.com/$GH_USER/$REPO.git"
	rc=0
	set_topics || rc=1
	enable_pages
	push_tags || rc=1
	show_remote
	exit $rc
fi

gh repo view "$GH_USER/$REPO" >/dev/null 2>&1 && { echo "$GH_USER/$REPO already exists on GitHub (to add only topics and tags: --sync)"; exit 1; }

# README: placeholder -> your GitHub user
grep -q SEU_USUARIO README.md && sed -i.bak "s/SEU_USUARIO/$GH_USER/g" README.md && rm -f README.md.bak

# Local repository
if [ -d .git ]; then
	echo "existing git history kept ($(git rev-list --count HEAD 2>/dev/null || echo 0) commits)"
	git add -A
	msg="${MSG:-$REPO v$VER}"
else
	git init -q -b main
	git add -A
	msg="${MSG:-$REPO v$VER}"
fi

# Local data that must not be published (listed in .gitignore, but tracked by older commits)
git rm -r -q --cached --ignore-unmatch configurations_backup logs

# Safety check: nothing generated by the build may go into the commit
bad=$(git ls-files | grep -E '^(build[^/]*|logs|dist)/|^app_flutter/(build|\.dart_tool)/|\.(elf|hex|o)$' || true)
if [ -n "$bad" ]; then
	echo "ERROR: generated files tracked by git (check .gitignore, then 'git rm -r --cached <path>'):"
	echo "$bad" | head -20
	git reset -q
	exit 1
fi
for f in CMakeLists.txt app/node_build.h scripts/build.sh web/index.html lib/LoRaMac-node/src/mac/LoRaMac.c .github/workflows/build.yml; do
	git ls-files --error-unmatch "$f" >/dev/null 2>&1 || { echo "ERROR: $f is missing from the repository"; git reset -q; exit 1; }
done

if ! git diff --cached --quiet 2>/dev/null || ! git rev-parse -q --verify HEAD >/dev/null; then
	echo "$(git diff --cached --name-only | wc -l) files in the commit"
	git commit -q -m "$msg"
fi

# GitHub: create, point "origin" to it and push the current branch
git remote get-url origin >/dev/null 2>&1 && git remote remove origin
rc=0
create_repo || rc=$?
[ "$rc" -eq 0 ] || exit "$rc"
if [ "$(gh config get git_protocol 2>/dev/null)" = ssh ]; then
	git remote add origin "git@github.com:$GH_USER/$REPO.git"
else
	git remote add origin "https://github.com/$GH_USER/$REPO.git"
fi
push_branch
# Topics and tags never stop the script: a failure is reported and --sync repeats them.
rc=0
set_topics || rc=1
enable_pages
push_tags always || rc=1
show_remote
exit $rc
