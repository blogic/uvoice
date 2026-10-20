#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
#
# uvoice - native voice stack for OpenWrt
# Copyright (C) 2026 Adtran Inc.
# Author: Chad Monroe <chad.monroe@adtran.com>
#
# Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
"""Validate one ubus capture against the uvoice API schema.

    schema_check.py <schema.json> [capture.json]

The capture is one envelope {object, method, args, reply}, which is what the
root of uvoice/schema/uvoice-api-schema.json validates. It arrives on stdin when
the second argument is absent.

Exit codes: 0 the capture validates, 1 it does not, 2 the check could not run.
A capture that does not validate prints one line to stderr: the path of the
error and its message. A capture whose object and method name no method of
the schema prints that pair, because no method of the schema speaks for it.

The dialect comes from the $schema keyword of the file, so the schema chooses
its own validator.

The only dependency is the jsonschema module (>= 4.18 for the 2020-12
dialect), which uvoice/schema/README.md already names as the way to validate a
capture.
"""

import json
import sys

DISCRIMINATORS = ("object", "method")


def json_read(path):
    if path is None:
        return json.load(sys.stdin)
    with open(path) as handle:
        return json.load(handle)


def error_path_get(error):
    if not error.absolute_path:
        return "<envelope>"
    return "/".join(str(part) for part in error.absolute_path)


def branches_get(error):
    branches = {}
    for sub in error.context:
        index = sub.schema_path[0] if sub.schema_path else -1
        branches.setdefault(index, []).append(sub)
    return branches


def branch_rank(errors):
    return len(errors), -max(len(item.absolute_path) for item in errors)


def discriminator_failed(errors):
    """Did this oneOf branch refuse the object or the method itself?

    A branch that names another method fails the const of one of the two. A
    capture that carries no method at all fails their required rule in every
    branch. Neither branch is the one the capture meant, so neither may hold
    the error the caller reads.
    """
    for item in errors:
        path = list(item.absolute_path)
        if item.validator == "const" and path and path[0] in DISCRIMINATORS:
            return True
        if item.validator != "required" or path:
            continue
        if not isinstance(item.instance, dict):
            return True
        missing = set(item.validator_value) - set(item.instance)
        if missing.intersection(DISCRIMINATORS):
            return True
    return False


def error_narrow(error):
    """Descend a oneOf into the branch the capture meant.

    The root of the schema is a oneOf over every method, so the validator
    reports one error there and its message carries the whole capture.
    jsonschema's own best_match stops at that root, because the const
    failures of the other branches tie on relevance. The branch this capture
    meant is the one that kept both discriminators: it fails on the property
    that broke instead. Fewest errors, then the deepest path, separate the
    rest.

    A capture whose object and method pair names no method of the schema
    leaves no such branch. The error keeps its context then, and the caller
    reports the pair rather than a property of a method the capture never
    named.
    """
    while error.context:
        branches = [errors for errors in branches_get(error).values()
                    if not discriminator_failed(errors)]
        if not branches:
            return error
        errors = min(branches, key=branch_rank)
        error = max(errors, key=lambda item: len(item.absolute_path))
    return error


def discriminator_render(capture, name):
    if not isinstance(capture, dict) or name not in capture:
        return "(absent)"
    return repr(capture[name])


def main(argv):
    if len(argv) < 1 or len(argv) > 2:
        sys.stderr.write("usage: schema_check.py <schema.json> [capture.json]\n")
        return 2

    try:
        from jsonschema import exceptions, validators
    except ImportError as exc:
        sys.stderr.write("no jsonschema module: %s\n" % exc)
        return 2

    try:
        schema = json_read(argv[0])
    except (OSError, ValueError) as exc:
        sys.stderr.write("cannot read the schema %s: %s\n" % (argv[0], exc))
        return 2

    capture_path = argv[1] if len(argv) == 2 else None
    try:
        capture = json_read(capture_path)
    except OSError as exc:
        sys.stderr.write("cannot read the capture %s: %s\n" % (capture_path, exc))
        return 2
    except ValueError as exc:
        sys.stderr.write("the capture is not JSON: %s\n" % exc)
        return 1

    validator_class = validators.validator_for(schema)
    try:
        validator_class.check_schema(schema)
    except exceptions.SchemaError as exc:
        sys.stderr.write("the schema does not validate: %s\n" % exc.message)
        return 2

    errors = sorted(validator_class(schema).iter_errors(capture),
                    key=lambda item: list(str(part)
                                          for part in item.absolute_path))
    if not errors:
        return 0

    error = error_narrow(errors[0])
    if error.context:
        sys.stderr.write("%s: object %s and method %s name no method of the "
                         "schema\n" %
                         (error_path_get(error),
                          discriminator_render(capture, "object"),
                          discriminator_render(capture, "method")))
        return 1

    sys.stderr.write("%s: %s\n" % (error_path_get(error), error.message))
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
