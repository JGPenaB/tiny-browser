window.throwsRuns = (window.throwsRuns || 0) + 1;
throw new RangeError('thrown at top level');
