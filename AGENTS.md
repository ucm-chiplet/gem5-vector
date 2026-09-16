# Repository Guidelines

## Project Structure & Module Organization

gem5 is a simulator repository with mixed C++, Python, SCons, and test assets.
Core simulator code lives in `src/`; public headers are in `include/`.
Configuration scripts and examples are in `configs/`. Build-system support is
split between `SConstruct`, `site_scons/`, `build_tools/`, and `build_opts/`.
System-level, PyUnit, and TestLib assets live under `tests/`; C++ unit tests
are usually colocated with their source under `src/`. Utility scripts are in
`util/`, and vendored third-party or integration code is under `ext/`.

Sí: ajustaría la sección para que todos los documentos sean la referencia y para exigir propuesta/aprobación previa. Markdown revisado: encabezados, listas y bloques están correctamente separados.


## Trabajo en la VPU Ara-like

La VPU vive en `src/cpu/vector_engine/`. Es un coprocesador vectorial
Ara-like conectado a `MinorCPU` para ejecutar instrucciones RVV mediante
offload.

La referencia funcional, arquitectónica y de diseño de la VPU son todos los
archivos de `documentacion/`. Antes de trabajar en la VPU, revisa los
documentos relacionados con el cambio. En especial, usa
`documentacion/Interfaces propuestas.md` para entender las responsabilidades
de cada interfaz y módulo.

### Propuesta y aprobación antes de modificar

Antes de crear, borrar o modificar cualquier archivo relacionado con la VPU,
presenta una propuesta completa y revisable al usuario. No hagas cambios hasta
recibir una aprobación explícita.

La propuesta debe indicar, como mínimo:

- El objetivo concreto y el comportamiento que se quiere conseguir.
- Los archivos y directorios que se crearían o modificarían.
- Los módulos afectados y sus responsabilidades.
- Las interfaces, datos y señales que cruzan entre módulos.
- Las decisiones de diseño relevantes y las alternativas descartadas.
- Qué queda fuera de alcance en esta iteración.
- Cómo se comprobaría el cambio, si procede.

Si la propuesta cambia durante la implementación, detente, explica el motivo y
pide una nueva aprobación antes de ampliar el alcance.

### Alcance del baseline

La primera versión debe ser pequeña, comprensible y funcional. El objetivo
inicial es ejecutar, en modo SE, `vsetvli`, `vle32.v`, `vadd.vv`, `vadd.vx` y
`vse32.v`, con enteros de 32 bits y accesos unit-stride.

En esta fase se prioriza la corrección funcional. El rendimiento, el
solapamiento de instrucciones, el modelado detallado de ciclos, el chaining y
la microarquitectura avanzada quedan fuera de alcance salvo aprobación expresa.

Mantén estas decisiones de diseño:

- `MinorCPU` decodifica la instrucción. La VPU recibe un `VectorCommand` y no
  debe volver a decodificar opcodes ni depender de `StaticInst` o `DynInst`.

- `vsetvli` se ejecuta en la CPU y actualiza el estado RVV. Las instrucciones
  vectoriales soportadas se envían a la VPU.

- El frontend admite comandos en orden y, en el baseline, sólo puede haber una
  instrucción vectorial activa.

- La aceptación de un comando y su finalización son eventos distintos.

- La identidad debe viajar de extremo a extremo: `CommandKey` para comandos,
  `taskId` para trabajo interno y `requestId` para peticiones de memoria.

- El baseline usa referencias arquitectónicas a registros (`VectorRegRef`).
  No introduzcas renombramiento físico, ROB ni estructuras de disponibilidad
  activas sin una propuesta y aprobación específicas.

### Organización del código

La estructura prevista es:

    src/cpu/vector_engine/
    ├── interface/      # frontera MinorCPU ↔ VPU
    ├── frontend/       # cola, secuenciador y reparto de tareas
    ├── common/         # tipos compartidos e invariantes
    ├── vpu/
    │   ├── lanes/      # lanes y ALU
    │   ├── register_file/
    │   ├── vlsu/       # cargas, stores e interfaz de memoria
    │   └── interconnect/
    └── future/         # extensiones aún no activas

Los directorios de `future/` reservan extensiones como renombramiento, ROB,
issue queues, SLDU y MASKU. No añadas lógica activa en ellos hasta que el
baseline funcional esté terminado y el usuario lo apruebe.

No copies automáticamente el diseño de Vitruvius. Puede servir como inspiración
para organizar el código, pero las decisiones funcionales deben seguir los
documentos de este proyecto y RVV 1.0.

### Pruebas y validación

No implementes pruebas por iniciativa propia si el usuario no las ha pedido.
Las pruebas del proyecto serán definidas por el equipo humano.

Aun así, si un cambio carece de una forma clara de validarse, incluye en la
propuesta una sugerencia breve de prueba o benchmark. Debe ser una propuesta,
no una implementación, salvo que el usuario autorice crearla.

Al finalizar un cambio aprobado, informa de qué se modificó, qué no se llegó a
modificar y qué comprobaciones se realizaron.


## Dependency Policy

The project policy for dependency-support updates is to cover Ubuntu LTS
releases still in standard support. The oldest supported LTS sets ordinary
dependency minimums, while dependencies from the newest LTS must also work.
Non-compiler floors should normally be written as `major.minor+`; compiler
policy uses GCC and Clang major-version ranges.

Policy intent is not proof of current coverage. Before claiming a release,
compiler, or dependency is supported, verify the checks in `SConstruct`, the
workflow matrices, Docker images and bake targets, and user documentation.

## Build, Test, and Development Commands

- `scons build/ALL/gem5.opt -j <jobs>`: build the optimized gem5 binary with
  all ISA targets.
- `scons build/X86/gem5.opt`: build with the `build_opts/X86` configuration;
  replace `X86` with another configuration name from `build_opts/`.
- `scons build/ALL/unittests.opt`: build and run C++ GoogleTest unit tests.
- `cd tests && ./main.py run -j <jobs>`: run the default quick TestLib tests.
- `cd tests && ../build/ALL/gem5.opt run_pyunit.py`: run Python unit tests
  after building `build/ALL/gem5.opt`.
- `cd tests && ./main.py run --length=long`: run longer TestLib suites; use
  `--length=very-long` only for broad validation.

## Coding Style & Naming Conventions

Use 4 spaces, no tabs, no trailing whitespace, and a 79-column line limit.
C++ style is enforced by `.clang-format` and gem5 hooks; use upper camel case
for classes, lower camel case for functions and members, snake case for local
variables and parameters, and all-caps macros. Python is formatted with Black
and imports are managed by isort using the settings in `pyproject.toml`.
Install hooks with:

```sh
pip install pre-commit
pre-commit install
```

Keep the hooks installed and active so commit-time errors can be fixed locally
before pushing PR updates to GitHub.

## Testing Guidelines

gem5 has three major local test layers. C++ GTests are usually `*.test.cc`
files under `src/` and are built and run through SCons targets such as
`build/ALL/unittests.opt` or matching `*.test.opt` binaries. Python unittest
files live under `tests/pyunit` and run through `tests/run_pyunit.py` from the
`tests` directory. `tests/gem5/pyunit/test_run.py` registers them as a quick
TestLib suite. Broader regression coverage uses TestLib, whose framework is in
`ext/testlib` and whose suites are mostly under `tests/gem5`.

TestLib suites are tagged by duration. `quick` is the default and is expected
for most pull requests. `long` covers heavier daily-style validation and runs
as part of the `daily-tests.yaml` workflow.
`very-long` is for tests that may take days and should be reserved for
release-level or high-risk changes. Use `./main.py list -q --suites` to find
suites and `./main.py run --uid <SuiteUID> --skip-build` for focused reruns.
Only use `--skip-build` when the required binaries already exist.

## CI Workflows

GitHub Actions live in `.github/workflows`. `ci-tests.yaml` runs pull request
checks, including pre-commit, clang-format, unit tests, builds, and quick
TestLib execution. `daily-tests.yaml` runs long TestLib coverage, extra daily
tests, unittests, and cache-warming builds. `weekly-tests.yaml` runs quick,
long, and very-long TestLib suites plus coverage upload. `compiler-tests.yaml`
validates supported GCC/Clang and dependency-image build configurations.

## Validation Tiers

Match validation to the edit. For docs-only changes, run `git diff --check`.
For Python or SCons files, `python3 -m py_compile <files>` checks syntax only.
`scons -Q --help` evaluates the top-level `SConstruct` and imported site setup,
but without a target it does not read build-specific `SConsopts` or
`SConscript` files, run configure probes, or validate source registration.
Use a narrow explicit SCons target when those behaviors may be affected. For
C++ edits, prefer targeted object or unit-test builds before broad binaries.
For TestLib changes, start with `./main.py list ... -q` to confirm selection,
then run the narrowest relevant suite; use `--skip-build` only when all
required binaries already exist.

## Commit & Pull Request Guidelines

Develop changes on branches based on `develop`, not `stable`. Commit subjects
use gem5 component tags from `MAINTAINERS.yaml`, for example
`tests,base: Add coverage for bit helpers`; keep the subject under 65
characters and body lines under 72 characters. Preserve blank lines between
body paragraphs and backtick literal code identifiers, commands, and paths.
Prefer small, focused commits and include relevant GitHub issue links. Pull
requests target `gem5/gem5:develop`, should explain the change and validation
performed, and must pass CI and review before merge.

Before broad or cross-subsystem edits, inspect `MAINTAINERS.yaml` for component
tags, maintainers, experts, and orphaned status. Use those tags in commit
subjects and use the ownership information to frame PRs and likely reviewers.
Follow the user's explicit instructions about committing and pushing; never
rewrite shared history or disrupt another contributor's work unless explicitly
directed.

New source files should carry an appropriate copyright notice and license
header. Follow the 3-Clause BSD license in `LICENSE` unless the contributor's
copyright holder or institutional requirements call for another repository-
accepted header; do not infer a copyright holder from nearby files.

## Release Process

The documented gem5 release process targets three releases per year.
Maintainers announce the release window, create `release-staging-{VERSION}`
from `develop`, run the full test suite, then merge the staging branch to
`stable` when ready. The
stable branch is tagged as `v{YY}.{MAJOR}.{MINOR}.{HOTFIX}`. During staging,
target normal work at `develop`; submit to staging only for release-critical
fixes. Hotfixes branch from `stable`, require normal review, merge back to both
`stable` and `develop`, and receive an incremented hotfix tag.

## Agent-Specific Notes

Preserve user constraints. If asked not to compile or run simulations, stay
with source, logs, artifacts, and metadata. For CI failures, inspect the actual
failing job logs before assigning cause; SCons configure failures often require
reading `build/ALL/gem5.build/scons_config.log`, not just the headline error.

Every new source file should use the copyright holder required by the
contributor's institution or other obligation. Avoid editing `gem5/ext/` for
policy or dependency updates; prefer build logic, Dockerfiles, workflows, and
documentation.

For dependency or compiler-support maintenance, keep `SConstruct`,
`.github/workflows`, `util/dockerfiles`, and docs aligned with the Ubuntu LTS
policy above.

Docker images provide standardized environments for many gem5 tests. Exact
historical reproduction also requires the workflow revision and the image
digest used by the failing job because a `:latest` tag can change. Ubuntu
all-dependency images are the baseline for many CI jobs. To reproduce a
failure, inspect the relevant workflow and use its image where practical. GPU
SE-mode CI uses the `gcn-gpu` image.

A GPU container alone does not select GPU TestLib suites; use `--host gcn_gpu`.
Current GPU suites also declare `VEGA_X86`, so the host-only selector resolves
to that build today. Add `--isa=VEGA_X86` when the task must explicitly lock
the target, and always confirm selection with `./main.py list ... -q` before
an expensive run.

Do not edit generated build outputs under `build/`. When generated files look
wrong, change the source generator, SCons rule, SLICC input, or SimObject
definition that produces them.

Common investigation paths: for SCons configure failures, read
`build/ALL/gem5.build/scons_config.log`; for GitHub Actions, inspect the
specific failing job rather than only the workflow conclusion; for long test
failures, use harness metadata such as `results.xml` before estimating rerun
cost from suite wall time.

Useful research sources, in rough order, are the current codebase and commit
history; GitHub PRs, issues, and Discussions; public archives for
`gem5-dev@gem5.org` and `gem5-users@gem5.org`; the gem5 website and its linked
resources; and comments or documentation embedded near the relevant source
code.

Concrete sources:
`https://github.com/gem5/gem5`,
`https://github.com/gem5/gem5/pulls`,
`https://github.com/gem5/gem5/issues`,
`https://github.com/orgs/gem5/discussions`,
`https://www.mail-archive.com/gem5-dev%40gem5.org/`,
`https://www.mail-archive.com/gem5-users%40gem5.org/`, and
`https://www.gem5.org/`.
