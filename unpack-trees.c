#include "cache.h"
#include "dir.h"
#include "tree.h"
#include "tree-walk.h"
#include "cache-tree.h"
#include "unpack-trees.h"
#include "progress.h"
#include "refs.h"

static inline void remove_entry(int remove)
{
	if (remove >= 0)
		remove_cache_entry_at(remove);
}

/* Unlink the last component and attempt to remove leading
 * directories, in case this unlink is the removal of the
 * last entry in the directory -- empty directories are removed.
 */
static void unlink_entry(char *name, char *last_symlink)
{
	char *cp, *prev;

	if (has_symlink_leading_path(name, last_symlink))
		return;
	if (unlink(name))
		return;
	prev = NULL;
	while (1) {
		int status;
		cp = strrchr(name, '/');
		if (prev)
			*prev = '/';
		if (!cp)
			break;

		*cp = 0;
		status = rmdir(name);
		if (status) {
			*cp = '/';
			break;
		}
		prev = cp;
	}
}

static struct checkout state;
static void check_updates(struct unpack_trees_options *o)
{
	unsigned cnt = 0, total = 0;
	struct progress *progress = NULL;
	char last_symlink[PATH_MAX];
	int i;

	if (o->update && o->verbose_update) {
		for (total = cnt = 0; cnt < active_nr; cnt++) {
			struct cache_entry *ce = active_cache[cnt];
			if (ce->ce_flags & (CE_UPDATE | CE_REMOVE))
				total++;
		}

		progress = start_progress_delay("Checking out files",
						total, 50, 1);
		cnt = 0;
	}

	*last_symlink = '\0';
	for (i = 0; i < active_nr; i++) {
		struct cache_entry *ce = active_cache[i];

		if (ce->ce_flags & (CE_UPDATE | CE_REMOVE))
			display_progress(progress, ++cnt);
		if (ce->ce_flags & CE_REMOVE) {
			if (o->update)
				unlink_entry(ce->name, last_symlink);
			remove_cache_entry_at(i);
			i--;
			continue;
		}
		if (ce->ce_flags & CE_UPDATE) {
			ce->ce_flags &= ~CE_UPDATE;
			if (o->update) {
				checkout_entry(ce, &state, NULL);
				*last_symlink = '\0';
			}
		}
	}
	stop_progress(&progress);
}

static inline int call_unpack_fn(struct cache_entry **src, struct unpack_trees_options *o, int remove)
{
	int ret = o->fn(src, o, remove);
	if (ret > 0) {
		o->pos += ret;
		ret = 0;
	}
	return ret;
}

static int unpack_index_entry(struct cache_entry *ce, struct unpack_trees_options *o)
{
	struct cache_entry *src[5] = { ce, };
	if (ce_stage(ce)) {
		if (o->skip_unmerged) {
			o->pos++;
		} else {
			remove_entry(o->pos);
		}
		return 0;
	}
	return call_unpack_fn(src, o, o->pos);
}

int traverse_trees_recursive(int n, unsigned long dirmask, unsigned long df_conflicts, struct name_entry *names, struct traverse_info *info)
{
	int i;
	struct tree_desc t[3];
	struct traverse_info newinfo;
	struct name_entry *p;

	p = names;
	while (!p->mode)
		p++;

	newinfo = *info;
	newinfo.prev = info;
	newinfo.name = *p;
	newinfo.pathlen += tree_entry_len(p->path, p->sha1) + 1;
	newinfo.conflicts |= df_conflicts;

	for (i = 0; i < n; i++, dirmask >>= 1) {
		const unsigned char *sha1 = NULL;
		if (dirmask & 1)
			sha1 = names[i].sha1;
		fill_tree_descriptor(t+i, sha1);
	}
	traverse_trees(n, t, &newinfo);
	return 0;
}

/*
 * Compare the traverse-path to the cache entry without actually
 * having to generate the textual representation of the traverse
 * path.
 *
 * NOTE! This *only* compares up to the size of the traverse path
 * itself - the caller needs to do the final check for the cache
 * entry having more data at the end!
 */
static int do_compare_entry(const struct cache_entry *ce, const struct traverse_info *info, const struct name_entry *n)
{
	int len, pathlen, ce_len;
	const char *ce_name;

	if (info->prev) {
		int cmp = do_compare_entry(ce, info->prev, &info->name);
		if (cmp)
			return cmp;
	}
	pathlen = info->pathlen;
	ce_len = ce_namelen(ce);

	/* If ce_len < pathlen then we must have previously hit "name == directory" entry */
	if (ce_len < pathlen)
		return -1;

	ce_len -= pathlen;
	ce_name = ce->name + pathlen;

	len = tree_entry_len(n->path, n->sha1);
	return df_name_compare(ce_name, ce_len, S_IFREG, n->path, len, n->mode);
}

static int compare_entry(const struct cache_entry *ce, const struct traverse_info *info, const struct name_entry *n)
{
	int cmp = do_compare_entry(ce, info, n);
	if (cmp)
		return cmp;

	/*
	 * Even if the beginning compared identically, the ce should
	 * compare as bigger than a directory leading up to it!
	 */
	return ce_namelen(ce) > traverse_path_len(info, n);
}

static struct cache_entry *create_ce_entry(const struct traverse_info *info, const struct name_entry *n, int stage)
{
	int len = traverse_path_len(info, n);
	struct cache_entry *ce = xcalloc(1, cache_entry_size(len));

	ce->ce_mode = create_ce_mode(n->mode);
	ce->ce_flags = create_ce_flags(len, stage);
	hashcpy(ce->sha1, n->sha1);
	make_traverse_path(ce->name, info, n);

	return ce;
}

static int unpack_nondirectories(int n, unsigned long mask, unsigned long dirmask, struct cache_entry *src[5],
	const struct name_entry *names, const struct traverse_info *info, int remove)
{
	int i;
	struct unpack_trees_options *o = info->data;
	unsigned long conflicts;

	/* Do we have *only* directories? Nothing to do */
	if (mask == dirmask && !src[0])
		return 0;

	conflicts = info->conflicts;
	if (o->merge)
		conflicts >>= 1;
	conflicts |= dirmask;

	/*
	 * Ok, we've filled in up to any potential index entry in src[0],
	 * now do the rest.
	 */
	for (i = 0; i < n; i++) {
		int stage;
		unsigned int bit = 1ul << i;
		if (conflicts & bit) {
			src[i + o->merge] = o->df_conflict_entry;
			continue;
		}
		if (!(mask & bit))
			continue;
		if (!o->merge)
			stage = 0;
		else if (i + 1 < o->head_idx)
			stage = 1;
		else if (i + 1 > o->head_idx)
			stage = 3;
		else
			stage = 2;
		src[i + o->merge] = create_ce_entry(info, names + i, stage);
	}

	if (o->merge)
		return call_unpack_fn(src, o, remove);

	n += o->merge;
	remove_entry(remove);
	for (i = 0; i < n; i++)
		add_cache_entry(src[i], ADD_CACHE_OK_TO_ADD|ADD_CACHE_SKIP_DFCHECK);
	return 0;
}

static int unpack_callback(int n, unsigned long mask, unsigned long dirmask, struct name_entry *names, struct traverse_info *info)
{
	struct cache_entry *src[5] = { NULL, };
	struct unpack_trees_options *o = info->data;
	int remove = -1;
	const struct name_entry *p = names;

	/* Find first entry with a real name (we could use "mask" too) */
	while (!p->mode)
		p++;

	/* Are we supposed to look at the index too? */
	if (o->merge) {
		while (o->pos < active_nr) {
			struct cache_entry *ce = active_cache[o->pos];
			int cmp = compare_entry(ce, info, p);
			if (cmp < 0) {
				if (unpack_index_entry(ce, o) < 0)
					return -1;
				continue;
			}
			if (!cmp) {
				if (ce_stage(ce)) {
					/*
					 * If we skip unmerged index entries, we'll skip this
					 * entry *and* the tree entries associated with it!
					 */
					if (o->skip_unmerged)
						return mask;
					remove_entry(o->pos);
					continue;
				}
				src[0] = ce;
				remove = o->pos;
			}
			break;
		}
	}

	if (unpack_nondirectories(n, mask, dirmask, src, names, info, remove) < 0)
		return -1;

	/* Now handle any directories.. */
	if (dirmask) {
		unsigned long conflicts = mask & ~dirmask;
		if (o->merge) {
			conflicts <<= 1;
			if (src[0])
				conflicts |= 1;
		}
		traverse_trees_recursive(n, dirmask, conflicts, names, info);
		return mask;
	}

	return mask;
}

static int unpack_failed(struct unpack_trees_options *o, const char *message)
{
	if (!o->gently) {
		if (message)
			return error(message);
		return -1;
	}
	discard_cache();
	read_cache();
	return -1;
}

int unpack_trees(unsigned len, struct tree_desc *t, struct unpack_trees_options *o)
{
	static struct cache_entry *dfc;

	if (len > 4)
		die("unpack_trees takes at most four trees");
	memset(&state, 0, sizeof(state));
	state.base_dir = "";
	state.force = 1;
	state.quiet = 1;
	state.refresh_cache = 1;

	o->merge_size = len;

	if (!dfc)
		dfc = xcalloc(1, sizeof(struct cache_entry) + 1);
	o->df_conflict_entry = dfc;

	if (len) {
		const char *prefix = o->prefix ? o->prefix : "";
		struct traverse_info info;

		setup_traverse_info(&info, prefix);
		info.fn = unpack_callback;
		info.data = o;

		if (traverse_trees(len, t, &info) < 0)
			return unpack_failed(o, NULL);
	}

	/* Any left-over entries in the index? */
	if (o->merge) {
		while (o->pos < active_nr) {
			struct cache_entry *ce = active_cache[o->pos];
			if (unpack_index_entry(ce, o) < 0)
				return unpack_failed(o, NULL);
		}
	}

	if (o->trivial_merges_only && o->nontrivial_merge)
		return unpack_failed(o, "Merge requires file-level merging");

	check_updates(o);
	return 0;
}

/* Here come the merge functions */

static int reject_merge(struct cache_entry *ce)
{
	return error("Entry '%s' would be overwritten by merge. Cannot merge.",
		     ce->name);
}

static int same(struct cache_entry *a, struct cache_entry *b)
{
	if (!!a != !!b)
		return 0;
	if (!a && !b)
		return 1;
	return a->ce_mode == b->ce_mode &&
	       !hashcmp(a->sha1, b->sha1);
}


/*
 * When a CE gets turned into an unmerged entry, we
 * want it to be up-to-date
 */
static int verify_uptodate(struct cache_entry *ce,
		struct unpack_trees_options *o)
{
	struct stat st;

	if (o->index_only || o->reset)
		return 0;

	if (!lstat(ce->name, &st)) {
		unsigned changed = ce_match_stat(ce, &st, CE_MATCH_IGNORE_VALID);
		if (!changed)
			return 0;
		/*
		 * NEEDSWORK: the current default policy is to allow
		 * submodule to be out of sync wrt the supermodule
		 * index.  This needs to be tightened later for
		 * submodules that are marked to be automatically
		 * checked out.
		 */
		if (S_ISGITLINK(ce->ce_mode))
			return 0;
		errno = 0;
	}
	if (errno == ENOENT)
		return 0;
	return o->gently ? -1 :
		error("Entry '%s' not uptodate. Cannot merge.", ce->name);
}

static void invalidate_ce_path(struct cache_entry *ce)
{
	if (ce)
		cache_tree_invalidate_path(active_cache_tree, ce->name);
}

/*
 * Check that checking out ce->sha1 in subdir ce->name is not
 * going to overwrite any working files.
 *
 * Currently, git does not checkout subprojects during a superproject
 * checkout, so it is not going to overwrite anything.
 */
static int verify_clean_submodule(struct cache_entry *ce, const char *action,
				      struct unpack_trees_options *o)
{
	return 0;
}

static int verify_clean_subdirectory(struct cache_entry *ce, const char *action,
				      struct unpack_trees_options *o)
{
	/*
	 * we are about to extract "ce->name"; we would not want to lose
	 * anything in the existing directory there.
	 */
	int namelen;
	int pos, i;
	struct dir_struct d;
	char *pathbuf;
	int cnt = 0;
	unsigned char sha1[20];

	if (S_ISGITLINK(ce->ce_mode) &&
	    resolve_gitlink_ref(ce->name, "HEAD", sha1) == 0) {
		/* If we are not going to update the submodule, then
		 * we don't care.
		 */
		if (!hashcmp(sha1, ce->sha1))
			return 0;
		return verify_clean_submodule(ce, action, o);
	}

	/*
	 * First let's make sure we do not have a local modification
	 * in that directory.
	 */
	namelen = strlen(ce->name);
	pos = cache_name_pos(ce->name, namelen);
	if (0 <= pos)
		return cnt; /* we have it as nondirectory */
	pos = -pos - 1;
	for (i = pos; i < active_nr; i++) {
		struct cache_entry *ce = active_cache[i];
		int len = ce_namelen(ce);
		if (len < namelen ||
		    strncmp(ce->name, ce->name, namelen) ||
		    ce->name[namelen] != '/')
			break;
		/*
		 * ce->name is an entry in the subdirectory.
		 */
		if (!ce_stage(ce)) {
			if (verify_uptodate(ce, o))
				return -1;
			ce->ce_flags |= CE_REMOVE;
		}
		cnt++;
	}

	/*
	 * Then we need to make sure that we do not lose a locally
	 * present file that is not ignored.
	 */
	pathbuf = xmalloc(namelen + 2);
	memcpy(pathbuf, ce->name, namelen);
	strcpy(pathbuf+namelen, "/");

	memset(&d, 0, sizeof(d));
	if (o->dir)
		d.exclude_per_dir = o->dir->exclude_per_dir;
	i = read_directory(&d, ce->name, pathbuf, namelen+1, NULL);
	if (i)
		return o->gently ? -1 :
			error("Updating '%s' would lose untracked files in it",
			      ce->name);
	free(pathbuf);
	return cnt;
}

/*
 * We do not want to remove or overwrite a working tree file that
 * is not tracked, unless it is ignored.
 */
static int verify_absent(struct cache_entry *ce, const char *action,
			 struct unpack_trees_options *o)
{
	struct stat st;

	if (o->index_only || o->reset || !o->update)
		return 0;

	if (has_symlink_leading_path(ce->name, NULL))
		return 0;

	if (!lstat(ce->name, &st)) {
		int cnt;
		int dtype = ce_to_dtype(ce);

		if (o->dir && excluded(o->dir, ce->name, &dtype))
			/*
			 * ce->name is explicitly excluded, so it is Ok to
			 * overwrite it.
			 */
			return 0;
		if (S_ISDIR(st.st_mode)) {
			/*
			 * We are checking out path "foo" and
			 * found "foo/." in the working tree.
			 * This is tricky -- if we have modified
			 * files that are in "foo/" we would lose
			 * it.
			 */
			cnt = verify_clean_subdirectory(ce, action, o);

			/*
			 * If this removed entries from the index,
			 * what that means is:
			 *
			 * (1) the caller unpack_trees_rec() saw path/foo
			 * in the index, and it has not removed it because
			 * it thinks it is handling 'path' as blob with
			 * D/F conflict;
			 * (2) we will return "ok, we placed a merged entry
			 * in the index" which would cause o->pos to be
			 * incremented by one;
			 * (3) however, original o->pos now has 'path/foo'
			 * marked with "to be removed".
			 *
			 * We need to increment it by the number of
			 * deleted entries here.
			 */
			o->pos += cnt;
			return 0;
		}

		/*
		 * The previous round may already have decided to
		 * delete this path, which is in a subdirectory that
		 * is being replaced with a blob.
		 */
		cnt = cache_name_pos(ce->name, strlen(ce->name));
		if (0 <= cnt) {
			struct cache_entry *ce = active_cache[cnt];
			if (ce->ce_flags & CE_REMOVE)
				return 0;
		}

		return o->gently ? -1 :
			error("Untracked working tree file '%s' "
			      "would be %s by merge.", ce->name, action);
	}
	return 0;
}

static int merged_entry(struct cache_entry *merge, struct cache_entry *old,
		struct unpack_trees_options *o)
{
	merge->ce_flags |= CE_UPDATE;
	if (old) {
		/*
		 * See if we can re-use the old CE directly?
		 * That way we get the uptodate stat info.
		 *
		 * This also removes the UPDATE flag on
		 * a match.
		 */
		if (same(old, merge)) {
			copy_cache_entry(merge, old);
		} else {
			if (verify_uptodate(old, o))
				return -1;
			invalidate_ce_path(old);
		}
	}
	else {
		if (verify_absent(merge, "overwritten", o))
			return -1;
		invalidate_ce_path(merge);
	}

	merge->ce_flags &= ~CE_STAGEMASK;
	add_cache_entry(merge, ADD_CACHE_OK_TO_ADD|ADD_CACHE_OK_TO_REPLACE);
	return 1;
}

static int deleted_entry(struct cache_entry *ce, struct cache_entry *old,
		struct unpack_trees_options *o)
{
	if (old) {
		if (verify_uptodate(old, o))
			return -1;
	} else
		if (verify_absent(ce, "removed", o))
			return -1;
	ce->ce_flags |= CE_REMOVE;
	add_cache_entry(ce, ADD_CACHE_OK_TO_ADD|ADD_CACHE_OK_TO_REPLACE);
	invalidate_ce_path(ce);
	return 1;
}

static int keep_entry(struct cache_entry *ce, struct unpack_trees_options *o)
{
	add_cache_entry(ce, ADD_CACHE_OK_TO_ADD);
	return 1;
}

#if DBRT_DEBUG
static void show_stage_entry(FILE *o,
			     const char *label, const struct cache_entry *ce)
{
	if (!ce)
		fprintf(o, "%s (missing)\n", label);
	else
		fprintf(o, "%s%06o %s %d\t%s\n",
			label,
			ce->ce_mode,
			sha1_to_hex(ce->sha1),
			ce_stage(ce),
			ce->name);
}
#endif

int threeway_merge(struct cache_entry **stages,
		struct unpack_trees_options *o,
		int remove)
{
	struct cache_entry *index;
	struct cache_entry *head;
	struct cache_entry *remote = stages[o->head_idx + 1];
	int count;
	int head_match = 0;
	int remote_match = 0;

	int df_conflict_head = 0;
	int df_conflict_remote = 0;

	int any_anc_missing = 0;
	int no_anc_exists = 1;
	int i;

	for (i = 1; i < o->head_idx; i++) {
		if (!stages[i] || stages[i] == o->df_conflict_entry)
			any_anc_missing = 1;
		else
			no_anc_exists = 0;
	}

	index = stages[0];
	head = stages[o->head_idx];

	if (head == o->df_conflict_entry) {
		df_conflict_head = 1;
		head = NULL;
	}

	if (remote == o->df_conflict_entry) {
		df_conflict_remote = 1;
		remote = NULL;
	}

	/* First, if there's a #16 situation, note that to prevent #13
	 * and #14.
	 */
	if (!same(remote, head)) {
		for (i = 1; i < o->head_idx; i++) {
			if (same(stages[i], head)) {
				head_match = i;
			}
			if (same(stages[i], remote)) {
				remote_match = i;
			}
		}
	}

	/* We start with cases where the index is allowed to match
	 * something other than the head: #14(ALT) and #2ALT, where it
	 * is permitted to match the result instead.
	 */
	/* #14, #14ALT, #2ALT */
	if (remote && !df_conflict_head && head_match && !remote_match) {
		if (index && !same(index, remote) && !same(index, head))
			return o->gently ? -1 : reject_merge(index);
		return merged_entry(remote, index, o);
	}
	/*
	 * If we have an entry in the index cache, then we want to
	 * make sure that it matches head.
	 */
	if (index && !same(index, head))
		return o->gently ? -1 : reject_merge(index);

	if (head) {
		/* #5ALT, #15 */
		if (same(head, remote))
			return merged_entry(head, index, o);
		/* #13, #3ALT */
		if (!df_conflict_remote && remote_match && !head_match)
			return merged_entry(head, index, o);
	}

	/* #1 */
	if (!head && !remote && any_anc_missing) {
		remove_entry(remove);
		return 0;
	}

	/* Under the new "aggressive" rule, we resolve mostly trivial
	 * cases that we historically had git-merge-one-file resolve.
	 */
	if (o->aggressive) {
		int head_deleted = !head && !df_conflict_head;
		int remote_deleted = !remote && !df_conflict_remote;
		struct cache_entry *ce = NULL;

		if (index)
			ce = index;
		else if (head)
			ce = head;
		else if (remote)
			ce = remote;
		else {
			for (i = 1; i < o->head_idx; i++) {
				if (stages[i] && stages[i] != o->df_conflict_entry) {
					ce = stages[i];
					break;
				}
			}
		}

		/*
		 * Deleted in both.
		 * Deleted in one and unchanged in the other.
		 */
		if ((head_deleted && remote_deleted) ||
		    (head_deleted && remote && remote_match) ||
		    (remote_deleted && head && head_match)) {
			remove_entry(remove);
			if (index)
				return deleted_entry(index, index, o);
			else if (ce && !head_deleted) {
				if (verify_absent(ce, "removed", o))
					return -1;
			}
			return 0;
		}
		/*
		 * Added in both, identically.
		 */
		if (no_anc_exists && head && remote && same(head, remote))
			return merged_entry(head, index, o);

	}

	/* Below are "no merge" cases, which require that the index be
	 * up-to-date to avoid the files getting overwritten with
	 * conflict resolution files.
	 */
	if (index) {
		if (verify_uptodate(index, o))
			return -1;
	}

	remove_entry(remove);
	o->nontrivial_merge = 1;

	/* #2, #3, #4, #6, #7, #9, #10, #11. */
	count = 0;
	if (!head_match || !remote_match) {
		for (i = 1; i < o->head_idx; i++) {
			if (stages[i] && stages[i] != o->df_conflict_entry) {
				keep_entry(stages[i], o);
				count++;
				break;
			}
		}
	}
#if DBRT_DEBUG
	else {
		fprintf(stderr, "read-tree: warning #16 detected\n");
		show_stage_entry(stderr, "head   ", stages[head_match]);
		show_stage_entry(stderr, "remote ", stages[remote_match]);
	}
#endif
	if (head) { count += keep_entry(head, o); }
	if (remote) { count += keep_entry(remote, o); }
	return count;
}

/*
 * Two-way merge.
 *
 * The rule is to "carry forward" what is in the index without losing
 * information across a "fast forward", favoring a successful merge
 * over a merge failure when it makes sense.  For details of the
 * "carry forward" rule, please see <Documentation/git-read-tree.txt>.
 *
 */
int twoway_merge(struct cache_entry **src,
		struct unpack_trees_options *o,
		int remove)
{
	struct cache_entry *current = src[0];
	struct cache_entry *oldtree = src[1];
	struct cache_entry *newtree = src[2];

	if (o->merge_size != 2)
		return error("Cannot do a twoway merge of %d trees",
			     o->merge_size);

	if (oldtree == o->df_conflict_entry)
		oldtree = NULL;
	if (newtree == o->df_conflict_entry)
		newtree = NULL;

	if (current) {
		if ((!oldtree && !newtree) || /* 4 and 5 */
		    (!oldtree && newtree &&
		     same(current, newtree)) || /* 6 and 7 */
		    (oldtree && newtree &&
		     same(oldtree, newtree)) || /* 14 and 15 */
		    (oldtree && newtree &&
		     !same(oldtree, newtree) && /* 18 and 19 */
		     same(current, newtree))) {
			return keep_entry(current, o);
		}
		else if (oldtree && !newtree && same(current, oldtree)) {
			/* 10 or 11 */
			remove_entry(remove);
			return deleted_entry(oldtree, current, o);
		}
		else if (oldtree && newtree &&
			 same(current, oldtree) && !same(current, newtree)) {
			/* 20 or 21 */
			return merged_entry(newtree, current, o);
		}
		else {
			/* all other failures */
			remove_entry(remove);
			if (oldtree)
				return o->gently ? -1 : reject_merge(oldtree);
			if (current)
				return o->gently ? -1 : reject_merge(current);
			if (newtree)
				return o->gently ? -1 : reject_merge(newtree);
			return -1;
		}
	}
	else if (newtree)
		return merged_entry(newtree, current, o);
	remove_entry(remove);
	return deleted_entry(oldtree, current, o);
}

/*
 * Bind merge.
 *
 * Keep the index entries at stage0, collapse stage1 but make sure
 * stage0 does not have anything there.
 */
int bind_merge(struct cache_entry **src,
		struct unpack_trees_options *o,
		int remove)
{
	struct cache_entry *old = src[0];
	struct cache_entry *a = src[1];

	if (o->merge_size != 1)
		return error("Cannot do a bind merge of %d trees\n",
			     o->merge_size);
	if (a && old)
		return o->gently ? -1 :
			error("Entry '%s' overlaps.  Cannot bind.", a->name);
	if (!a)
		return keep_entry(old, o);
	else
		return merged_entry(a, NULL, o);
}

/*
 * One-way merge.
 *
 * The rule is:
 * - take the stat information from stage0, take the data from stage1
 */
int oneway_merge(struct cache_entry **src,
		struct unpack_trees_options *o,
		int remove)
{
	struct cache_entry *old = src[0];
	struct cache_entry *a = src[1];

	if (o->merge_size != 1)
		return error("Cannot do a oneway merge of %d trees",
			     o->merge_size);

	if (!a) {
		remove_entry(remove);
		return deleted_entry(old, old, o);
	}
	if (old && same(old, a)) {
		if (o->reset) {
			struct stat st;
			if (lstat(old->name, &st) ||
			    ce_match_stat(old, &st, CE_MATCH_IGNORE_VALID))
				old->ce_flags |= CE_UPDATE;
		}
		return keep_entry(old, o);
	}
	return merged_entry(a, old, o);
}
