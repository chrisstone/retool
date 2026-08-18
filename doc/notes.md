1. Debugging flag, shows memory usage?

2. Memory usage on Inspect
	0.5-1TB for 200 files, 6.5TB of clusters

3. Output
	* field, table , and progress datatype enum with display text and format
	* field and table overloads

4. Known test coverage gaps (deferred - low priority relative to cost, or already
   indirectly covered by another test's assumptions)
	* Long paths (>260 chars) or Unicode filenames - wide APIs suggest intent to
	  support this, unverified.
	* Ctrl+C cancellation mid-copy/dedup (util::g_cancel_requested) - hard to
	  script reliably (needs a real signal to a running process, not just a
	  locked file), zero coverage today.
	* Disk-full mid-COPY specifically (as opposed to mid-dedup, which 41's tight
	  1GB volume setup already incidentally covers).
	* -i file list containing a mix of files/dirs/globs, a nonexistent path
	  entry, or paths containing spaces.
	* -c:O (Owner) copy component - copy.cpp does copy Owner/Primary Group SIDs
	  when O is selected, but verifying it correctly needs manipulating file
	  ownership (SeTakeOwnershipPrivilege, comparing SIDs across accounts),
	  meaningfully more fragile/environment-dependent than the -c:S (ACL) test
	  added in 58_copy_security_owner.ps1 for a comparatively small slice of
	  additional coverage.
	* Non-ReFS pair-wise dedup (`retool dedup C:\f1 C:\f2`) - hits the exact same
	  ReFS-required check (dedup.cpp:375-380) already covered for volume-wide
	  mode by 60_cli_validation.ps1; a second test would exercise the same code
	  path via a different CLI shape, not new logic.