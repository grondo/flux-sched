#!/bin/sh

test_description='Test that parent duration is inherited according to RFC14'

. `dirname $0`/sharness.sh

#
# test_under_flux is under sharness.d/
#
test_under_flux 1

export FLUX_URI_RESOLVE_LOCAL=t

test_expect_success HAVE_JQ 'parent expiration is inherited when duration=0' '
	cat >get_R.sh <<-EOT &&
	#!/bin/sh
	flux job info \$FLUX_JOB_ID R
	EOT
	chmod +x get_R.sh &&
	jobid=$(flux alloc -n1 -t5m --bg) &&
	expiration=$(flux job info $jobid R | jq .execution.expiration) &&
	test_debug "echo expiration of alloc job is $expiration" &&
	R1=$(flux proxy $jobid flux run -n1 ./get_R.sh) &&
	exp1=$(echo "$R1" | jq .execution.expiration) &&
	test_debug "echo expiration of job is $exp1" &&
	echo $exp1 | jq ". == $expiration" &&
	sleep 1 &&
	R1=$(flux proxy $jobid flux run -n1 ./get_R.sh) &&
	exp1=$(echo "$R1" | jq .execution.expiration) &&
	test_debug "echo expiration of second job is $exp1" &&
	echo $exp1 | jq ". == $expiration" &&
	flux shutdown --quiet $jobid
'

test_done
