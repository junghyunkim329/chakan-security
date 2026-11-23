"""Pure evidence predicates used by the privileged BodyControl integration scenario."""


def routing_rejection(stderr: str, runtime: str):
    combined = stderr + "\n" + runtime
    for marker in ("Permission denied", "Check credentials failed", "Check routing credentials failed"):
        if marker in combined:
            return marker
    lock_denied = "Could not open" in runtime and "errno 13" in runtime
    refused_manager = "configured as routing but other routing manager present. Won't instantiate routing" in runtime
    if lock_denied and refused_manager:
        return "routing lock permission denied"
    return None


def contiguous_positive_steps(samples, max_gap=0.15):
    """Return increases between checksum-valid samples close enough to represent adjacent fade observations."""
    return [
        value_b - value_a
        for (time_a, value_a), (time_b, value_b) in zip(samples, samples[1:])
        if 0 < time_b - time_a <= max_gap and value_b > value_a
    ]
