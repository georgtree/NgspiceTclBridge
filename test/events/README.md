# Raw event integration test fixtures

Source: ngspice mirror https://github.com/imr/ngspice, commit a80f6e3.

- `mixed-invert.cir` is `examples/xspice/original-examples/digital_invert.deck`, unchanged.
  It contains an analog pulse, ADC bridge, digital gates and DAC outputs.
- `digital-source.cir` adapts `examples/xspice/d_source/d_source.cir`: retain the eleven-output
  source, remove the two other sources and interactive control section, and add `.tran 100ps 30ns`.
  It has no analog circuit nodes. `d_source-stimulus.txt` is copied unchanged from the same directory.

Load the analog and digital code-model libraries, and set the bridge inputpath to this directory before loading
these netlists. `make test-events-real` does this automatically when NGSPICE_CODEMODEL_DIR is supplied.

Original license notices are in LICENSE.ngspice. The adaptation adds no restriction to the upstream examples.
