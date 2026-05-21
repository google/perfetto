# UI Testing Strategy

**Authors:** @stevegolton

**Status:** Draft

## Problem

UI testing is difficult. Testing whether the UI behaves, looks, and feels
correct is very difficult to describe to a machine.

We currently have two types of automated UI testsing that run on the CI:

- Unit tests (including jsdom tests)
- Integration tests based on playwright

The former is relatively benign. Usual unit testing practice applies here - pull
chucks of functionality out of the codebase and test it in isolation, avoiding
mocks as much as is practical.

I have recently introduced a jsdom test which tests the correctness of the
DataGrid component using jsdom. This is a slight departure from the current
state of tests which mainly focus on testing pure functions as it renders the
component into a virtua DOM (based on the jsdom library) and asserts that
certain elements are present. This is in fact how [mithril
recommends](https://mithril.js.org/testing.html) we test components, which is
reassuring.

Testing like this does, of course, have its downsides. We're constantly at risk
of over testing - testing implementation details rather than functionality.
After all, the job of a component is to push some intent into a user's eyeballs
and allow their internal pattern recognition system to understand easily what
the the things does and how they should interact with it to make the UI do
stuff, which is a far cry from asserting that a specific div exists and that it
is purple.

Integration tests use playwright to interact with the web page using a real
browser, and can make assertions about the state of the DOM but, crucially, it
can also take screnshots of either the full page or speicific elements on the
page. This latter technologuy is crucial for elements that have no real DOM
representation (e.g. canvas) and for asserting that styles look correct. The
only problem is these screenshot tests are resigned to 'known good' / golden
comparison tests - which only assert that the screenshots are as good as a
previous run where the screenshot happened to be captured.

[](https://storage.googleapis.com/perfetto-ci-artifacts/gh-26835408928-2-ui/ui-test-artifacts/index.html)

These tests are an incredibly blunt instrument. Due to the incresbly wide net a
screenshot test casts (it tests not only what you're trying to test but also the
properties of 100 other entities you weren't trying to test, resulting in a
problematic level of false positives. Take this for example - all this person
did is add a new plugin and the test that is used to check that.

I think we should lean on jsdom tests a lot more to test correctness of
components as a first line on defence.

We should also add a way of using trace processor in nodejs in tests in order to
load traces at test time. This will enable a whole category of test that
currently require a test trace to be built. We should also add a protobuf trace
building layer that can enable the easy creation of simple targetted protobuf
traces.
