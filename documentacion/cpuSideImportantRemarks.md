1. En el constructor de cpu vamos a usar VectorEngine o usaremos VpuCommandEndpoint? En el segundo caso, no necesitaríamos un .py, para poderlo pasar desde los archivos de configuración de nuestra simulación?

2. Se puede desactivar el offload y tener una cpuvectorial? Supongo que sí, por eso lo pongo como diferentes atributos de cpu.hh

3. VSETVL realmente lo dejamos ejecutar como se ejecuta actualmente, no? Y luego usamos `./arch/riscv/regs/misc.hh` + getMISCReg o como hacemos? Supongo que lo hacemos en commit justo cuando le vayamos a mandar la instrucción a la VPU, no?

4. ¿Como gestionamos la coherencia entre VPUOffload instructions y VPUNaive instructions? -> Se me ocurre tener una función o algo que permita insertar a piñon datos en el VRF(sin simular timing más que el ya modelado por gem5) pero igual es un poco intrusivo.

5. Relacionado con la 4, ¿realmente vamos a tener VPU Naive instructions?

6. cpuEndpoint será el cpu o Execute?(De momento estoy guardando la información necesaria en Execute, por eso digo).

7. Realmente después de WaitDependencies podemos hacer la parte de WaitGrant (Si no hay dependencias, claro). ¿Esperamos al siguiente commitInst, o lo hacemos seguido?

* **CAMBIOS REALIZADOS**:
  * Funciones añadidas:
    - `Scoreboard::canVPUInstOffload(MinorDynInstPtr inst, Cycles now, ThreadContext *thread_context)`: Se asegura de que todos los operandos escalares que necesita la instrucción estén disponibles. Se usa en commit y no en issue ya que sino frenaría el pipeline/ejecución escalar en caso de dependencia. Esto se hace para poder dejar el flujo de `markupInstDests` igual, es decir, las instrucciones vectoriales seguirán usando esa función y marcando que van a escribir en un vectorial. Se puede tomar la decisión de crear también una función que únicamente escriba en el scoreboard los registros escalares a los que va a acceder pero realmente no es necesario.
    - `Decode::decodeVectorOp(const StaticInstPtr &static_inst)`: Devuelve el DecodeVectorOp ya formateado. De momento está filtrando en bruto para controlar que solo las 4 instrucciones establecidas como primer foco de implementación y prueba pasen a la VPU.
    - `MinorCPU::isVectorOffloadEnabled()`: Simplemente devuelve si tiene activado vectorOffload o no.
    - `Execute::vpuOffloadStates`: Para guardar el estado de las instrucciones que están en inFlightInstructions. Es una cola ya que de momento no podemos mandar varias instrucciones a la VPU -> no recibiremos respuestas para instrucciones que no estén en la cabeza de inFlightInst.

  * Modificaciones(Atributos):
    - `MinorCPU::vectorOffloadEnabledFlag`: Variable que almacena si está activado el vectorOffload o no.
    - `MinorCPU::vpu`: Variable que almacena el VPU. No descomentada todavía.
    - `MinorDynInst::decodedVectorOp`: Variable que almacena la decodificación hecha para gestionar el offload a la VPU.

  * Modificaciones(Funciones):
    - `Decode::evaluate()`: Se añade la decodificación para soportar el VPU Offload y se evita que se divida una Macroop en microop, mandándo la instrucción completa a `Execute`.
    - `Execute::evaluate()`: En la parte donde se comprueba si el head of inFlightInst puede comitearse las comprobaciones difieren en función de la phase en la que se encuentre, e.g. si está en WaitDependencies, se comprueba si los operandos están listos con `canVPUInstOffload`. No es 100% necesario y se podría poner a true siempre pero lo pongo por mayor rendimiento.
    - `Execute::issue()`: Asignamos una UF ficticia como sucede en NoCostInst cuando es VPUOffload para que en cuanto llegue al head of inflightinst pueda lanzarse(si están sus operandos disponibles). No se comprueban dependencias, ya que eso lo haremos en commit. Por último, creamos y guardamos el `MinorVectorState` de nuestra instrucción VPU Offload
    - `Execute::commit()`: DPRINTF para ver que instrucciones son VPUOffload llegan a aquí y cuando (Prescindible though). Se trata como una FU-less instruction, tratando siempre de commitearla y posteriormente, en el `if(try_to_commit)` se comprueba si cumple los requisitos para ejecutar `commitInst`.
    - `Execute::commitInst()`: Capturamos las instrucciones que vayan a ir a la VPU y las procesamos. De esta manera, capturamos el MinorVectorState de esa instrucción: el state que esté en front deberá de ser matemáticamente el state de la instrucción VPUOffload más antigua en inFlightInst. Posteriormente haremos una cosa u otra en finción de la fase en la que se encuentre (especificado en documentación/Interfaces propuestas.md). Cuando llegue a completed se marcará como completada y borrará de la cola.
