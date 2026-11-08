/*
 * Copyright (c) 2013-2014 ARM Limited
 * All rights reserved
 *
 * The license below extends only to copyright in the software and shall
 * not be construed as granting a license to any other intellectual
 * property including but not limited to intellectual property relating
 * to a hardware implementation of the functionality of the software
 * licensed hereunder.  You may use the software subject to the license
 * terms below provided that you ensure that this notice is replicated
 * unmodified and in its entirety in all distributions of the software,
 * modified or unmodified, in source code or in binary form.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution;
 * neither the name of the copyright holders nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "cpu/minor/decode.hh"

#include "arch/generic/decoder.hh"
#include "config/use_riscv_isa.hh"

#if USE_RISCV_ISA
#include "arch/riscv/faults.hh"
#include "arch/riscv/insts/static_inst.hh"
#include "arch/riscv/insts/vector.hh"
#include "arch/riscv/utility.hh"

#endif
#include "base/logging.hh"
#include "base/trace.hh"
#include "cpu/minor/pipeline.hh"
#include "debug/Decode.hh"

namespace gem5
{

namespace minor
{

#if USE_RISCV_ISA
namespace
{

/**
 * Intenta decodificar una instrucción de memoria vectorial unit-stride
 * delegando en la infraestructura ISA de gem5.
 *
 * Retorna true si la instrucción fue reconocida y empaquetada como
 * VpuOffload en decoded, o false en caso contrario.
 */
bool
tryDecodeMemoryOp(const StaticInstPtr &static_inst,
                  const RiscvISA::RiscvStaticInst *rv_inst,
                  DecodedVectorOp &decoded)
{
    const auto *vle =
        dynamic_cast<const RiscvISA::VleMacroInst *>(static_inst.get());
    const auto *vse =
        dynamic_cast<const RiscvISA::VseMacroInst *>(static_inst.get());

    if (!vle && !vse) {
        return false;
    }

    DecodedMemoryOp memory;
    memory.direction = vle ? vector_engine::MemoryDirection::Load
                           : vector_engine::MemoryDirection::Store;
    memory.elementWidthBits = RiscvISA::width_EEW(rv_inst->machInst.width);

    if (vle) {
        memory.dataReg = static_inst->destRegIdx(0).index();
        memory.base.sourceOperandIndex = 0;
    } else {
        const bool base_first =
            static_inst->srcRegIdx(0).classValue() == IntRegClass;
        if (base_first) {
            memory.base.sourceOperandIndex = 0;
            memory.dataReg = static_inst->srcRegIdx(1).index();
        } else {
            memory.base.sourceOperandIndex = 1;
            memory.dataReg = static_inst->srcRegIdx(0).index();
        }
    }
    panic_if(memory.dataReg >= 32,
             "VPU memory operand is not an architectural vector register");

    decoded.executionClass = VectorExecutionClass::VpuOffload;
    decoded.payload = memory;
    return true;
}

/**
 * Intenta decodificar una instrucción aritmética vectorial soportada
 * delegando en la infraestructura ISA de gem5.
 *
 * Retorna true si la instrucción fue reconocida y empaquetada como
 * VpuOffload en decoded, o false en caso contrario.
 */
// Estructura auxiliar para clasificar operaciones aritméticas vectoriales
// reconocidas.
struct ArithmeticOpClassification
{
    vector_engine::ArithmeticOperation operation;
    vector_engine::ElementSignedness signedness;
};

constexpr uint64_t
hashString(std::string_view str)
{
    uint64_t hash = 14695981039346656037ULL;
    for (char c : str) {
        hash = (hash ^ static_cast<uint64_t>(c)) * 1099511628211ULL;
    }
    return hash;
}

/**
 * Catálogo auxiliar de instrucciones aritméticas vectoriales reconocidas para
 * offload hacia la VPU. Desacopla la lógica de decodificación del pipeline
 * de las variantes y mnemónicos de la ISA mediante un switch evaluado en
 * tiempo de compilación.
 */
static std::optional<ArithmeticOpClassification>
classifyVectorArithOp(const std::string_view name)
{
    using vector_engine::ArithmeticOperation;
    using vector_engine::ElementSignedness;

    switch (hashString(name)) {
        // Suma entera
        case hashString("vadd_vv"):
        case hashString("vadd_vx"):
            return ArithmeticOpClassification{
                ArithmeticOperation::Add, ElementSignedness::NotApplicable};

        // Multiplicación entera
        case hashString("vmul_vv"):
        case hashString("vmul_vx"):
            return ArithmeticOpClassification{
                ArithmeticOperation::Multiply,
                ElementSignedness::NotApplicable};

        // División entera (con y sin signo)
        case hashString("vdiv_vv"):
        case hashString("vdiv_vx"):
            return ArithmeticOpClassification{ArithmeticOperation::Divide,
                                              ElementSignedness::Signed};
        case hashString("vdivu_vv"):
        case hashString("vdivu_vx"):
            return ArithmeticOpClassification{ArithmeticOperation::Divide,
                                              ElementSignedness::Unsigned};

        // Resto entero (con y sin signo)
        case hashString("vrem_vv"):
        case hashString("vrem_vx"):
            return ArithmeticOpClassification{ArithmeticOperation::Remainder,
                                              ElementSignedness::Signed};
        case hashString("vremu_vv"):
        case hashString("vremu_vx"):
            return ArithmeticOpClassification{ArithmeticOperation::Remainder,
                                              ElementSignedness::Unsigned};

        // Coma flotante (FP32)
        case hashString("vfadd_vv"):
            return ArithmeticOpClassification{
                ArithmeticOperation::FloatAdd,
                ElementSignedness::NotApplicable};
        case hashString("vfmul_vv"):
            return ArithmeticOpClassification{
                ArithmeticOperation::FloatMultiply,
                ElementSignedness::NotApplicable};
        case hashString("vfdiv_vv"):
            return ArithmeticOpClassification{
                ArithmeticOperation::FloatDivide,
                ElementSignedness::NotApplicable};

        default:
            return std::nullopt;
    }
}

bool
tryDecodeArithmeticOp(const StaticInstPtr &static_inst,
                      DecodedVectorOp &decoded)
{
    // Verificar que es una macroinstrucción aritmética vectorial de RISC-V.
    const auto *arith_inst =
        dynamic_cast<const RiscvISA::VectorArithMacroInst *>(
            static_inst.get());
    if (!arith_inst) {
        return false;
    }

    // Clasificar la operación a partir del catálogo auxiliar.
    const auto classification = classifyVectorArithOp(static_inst->getName());
    if (!classification) {
        return false;
    }

    DecodedArithmeticOp arithmetic;
    arithmetic.operation = classification->operation;
    arithmetic.signedness = classification->signedness;
    arithmetic.destination = static_inst->destRegIdx(0).index();

    // En RVV 1.0: vd = vs2 op (vs1 | rs1).
    // En la codificación de gem5 (formats/vector_arith.isa):
    // - srcRegIdx(0) es vs1 (si .vv) o rs1 (si .vx).
    // - srcRegIdx(1) es vs2.
    // El primer operando (vectorSource) debe ser vs2 para respetar la
    // semántica no conmutativa en divisiones, restos y coma flotante.
    const bool src0_is_vec =
        (static_inst->srcRegIdx(0).classValue() == VecRegClass);
    const bool src1_is_vec =
        (static_inst->numSrcRegs() > 1 &&
         static_inst->srcRegIdx(1).classValue() == VecRegClass);

    if (src0_is_vec && src1_is_vec) {
        // Formato vector-vector (.vv): vs2 en src1, vs1 en src0
        arithmetic.vectorSource = static_inst->srcRegIdx(1).index();
        arithmetic.secondOperand = static_inst->srcRegIdx(0).index();
    } else if (src1_is_vec && !src0_is_vec) {
        // Formato vector-escalar (.vx): vs2 en src1, rs1 en src0
        arithmetic.vectorSource = static_inst->srcRegIdx(1).index();
        arithmetic.secondOperand = ScalarOperandRef{0};
    } else if (src0_is_vec && !src1_is_vec) {
        panic(
            "VPU arithmetic instruction '%s' has an unexpected operand layout",
            static_inst->getName());
    }

    panic_if(arithmetic.vectorSource >= 32,
             "VPU arithmetic source is not an architectural register");

    if (auto *reg = std::get_if<RegIndex>(&arithmetic.secondOperand)) {
        panic_if(*reg >= 32, "VPU arithmetic second source is not an "
                             "architectural register");
    }

    decoded.executionClass = VectorExecutionClass::VpuOffload;
    decoded.payload = arithmetic;
    return true;
}

/**
 * Clasifica la operación slide a partir del nemónico de la instrucción.
 * Se hace con hash en vez de dynamic_cast. O(1) en ejecución.
 */
std::optional<vector_engine::SlideOperation>
classifyVectorSlideOp(const std::string_view name)
{
    using vector_engine::SlideOperation;

    switch (hashString(name)) {
        case hashString("vslide1up_vx"):
            return SlideOperation::Slide1Up;
        case hashString("vslide1down_vx"):
            return SlideOperation::Slide1Down;
        default:
            return std::nullopt;
    }
}

/**
 * Intenta decodificar una instrucción vectorial de slide soportada
 * (vslide1up.vx, vslide1down.vx).
 *
 * Retorna true si la instrucción fue reconocida y empaquetada como
 * VpuOffload en decoded, o false en caso contrario.
 */
bool
tryDecodeSlideOp(const StaticInstPtr &static_inst,
                 const RiscvISA::RiscvStaticInst *rv_inst,
                 DecodedVectorOp &decoded)
{
    const auto op = classifyVectorSlideOp(static_inst->getName());
    if (!op) {
        return false;
    }

    DecodedSlideOp slide;
    slide.operation = *op;
    slide.destination = rv_inst->machInst.vd;
    slide.vectorSource = rv_inst->machInst.vs2;
    slide.scalar = ScalarOperandRef{0}; // rs1

    panic_if(slide.destination >= 32,
             "VPU slide destination is not an architectural register");
    panic_if(slide.vectorSource >= 32,
             "VPU slide source is not an architectural register");

    decoded.executionClass = VectorExecutionClass::VpuOffload;
    decoded.payload = slide;
    return true;
}

} // anonymous namespace
#endif

DecodedVectorOp
Decode::decodeVectorOp(const StaticInstPtr &static_inst)
{
    panic_if(!static_inst, "Cannot classify a null instruction");
    DecodedVectorOp decoded;
    if (!static_inst->isVector()) {
        return decoded;
    }
    if (static_inst->opClass() == OpClass::SimdConfig) {
        decoded.executionClass = VectorExecutionClass::CpuVectorConfig;
        return decoded;
    }
    if (!cpu.isVectorOffloadEnabled()) {
        decoded.executionClass = VectorExecutionClass::NativeVector;
        return decoded;
    }

#if USE_RISCV_ISA
    const auto *rv_inst =
        dynamic_cast<const RiscvISA::RiscvStaticInst *>(static_inst.get());
    panic_if(!rv_inst, "VPU offload requires a RISC-V instruction");

    // ExtMachInst expone directamente el bitfield de máscara.
    decoded.masked = !rv_inst->machInst.vm;

    // Reconocer operaciones de memoria unit-stride delegando en la ISA.
    if (tryDecodeMemoryOp(static_inst, rv_inst, decoded)) {
        return decoded;
    }

    // Reconocer operaciones aritméticas soportadas delegando en la ISA.
    if (tryDecodeArithmeticOp(static_inst, decoded)) {
        return decoded;
    }

    // Reconocer operaciones de desplazamiento (slide) soportadas.
    if (tryDecodeSlideOp(static_inst, rv_inst, decoded)) {
        return decoded;
    }

    decoded.executionClass = VectorExecutionClass::UnsupportedVector;
    return decoded;
#else
    panic("VPU offload requires RISC-V support");
#endif
}

Decode::Decode(const std::string &name,
    MinorCPU &cpu_,
    const BaseMinorCPUParams &params,
    Latch<ForwardInstData>::Output inp_,
    Latch<ForwardInstData>::Input out_,
    std::vector<InputBuffer<ForwardInstData>> &next_stage_input_buffer) :
    Named(name),
    cpu(cpu_),
    inp(inp_),
    out(out_),
    nextStageReserve(next_stage_input_buffer),
    outputWidth(params.executeInputWidth),
    processMoreThanOneInput(params.decodeCycleInput),
    decodeInfo(params.numThreads),
    threadPriority(0)
{
    if (outputWidth < 1)
        fatal("%s: executeInputWidth must be >= 1 (%d)\n", name, outputWidth);

    if (params.decodeInputBufferSize < 1) {
        fatal("%s: decodeInputBufferSize must be >= 1 (%d)\n", name,
        params.decodeInputBufferSize);
    }

    /* Per-thread input buffers */
    for (ThreadID tid = 0; tid < params.numThreads; tid++) {
        inputBuffer.push_back(
            InputBuffer<ForwardInstData>(
                name + ".inputBuffer" + std::to_string(tid), "insts",
                params.decodeInputBufferSize));
    }
}

const ForwardInstData *
Decode::getInput(ThreadID tid)
{
    /* Get insts from the inputBuffer to work with */
    if (!inputBuffer[tid].empty()) {
        const ForwardInstData &head = inputBuffer[tid].front();

        return (head.isBubble() ? NULL : &(inputBuffer[tid].front()));
    } else {
        return NULL;
    }
}

void
Decode::popInput(ThreadID tid)
{
    if (!inputBuffer[tid].empty())
        inputBuffer[tid].pop();

    decodeInfo[tid].inputIndex = 0;
    decodeInfo[tid].inMacroop = false;
}

#if TRACING_ON
/** Add the tracing data to an instruction.  This originates in
 *  decode because this is the first place that execSeqNums are known
 *  (these are used as the 'FetchSeq' in tracing data) */
static void
dynInstAddTracing(MinorDynInstPtr inst, StaticInstPtr static_inst,
    MinorCPU &cpu)
{
    inst->traceData = cpu.getTracer()->getInstRecord(curTick(),
        cpu.getContext(inst->id.threadId),
        inst->staticInst, *inst->pc, static_inst);

    /* Use the execSeqNum as the fetch sequence number as this most closely
     *  matches the other processor models' idea of fetch sequence */
    if (inst->traceData)
        inst->traceData->setFetchSeq(inst->id.execSeqNum);
}
#endif

void
Decode::evaluate()
{
    /* Push input onto appropriate input buffer */
    if (!inp.outputWire->isBubble())
        inputBuffer[inp.outputWire->threadId].setTail(*inp.outputWire);

    ForwardInstData &insts_out = *out.inputWire;

    assert(insts_out.isBubble());

    for (ThreadID tid = 0; tid < cpu.numThreads; tid++)
        decodeInfo[tid].blocked = !nextStageReserve[tid].canReserve();

    ThreadID tid = getScheduledThread();

    if (tid != InvalidThreadID) {
        DecodeThreadInfo &decode_info = decodeInfo[tid];
        const ForwardInstData *insts_in = getInput(tid);

        unsigned int output_index = 0;

        /* Pack instructions into the output while we can.  This may involve
         * using more than one input line */
        while (insts_in &&
           decode_info.inputIndex < insts_in->width() && /* Still more input */
           output_index < outputWidth /* Still more output to fill */)
        {
            MinorDynInstPtr inst = insts_in->insts[decode_info.inputIndex];

            if (inst->isBubble()) {
                /* Skip */
                decode_info.inputIndex++;
                decode_info.inMacroop = false;
            } else {
                StaticInstPtr static_inst = inst->staticInst;
                /* Static inst of a macro-op above the output_inst */
                StaticInstPtr parent_static_inst = NULL;
                if (!inst->isFault()) {
                    inst->decodedVectorOp = decodeVectorOp(static_inst);
#if USE_RISCV_ISA
                    if (inst->decodedVectorOp.executionClass ==
                        VectorExecutionClass::UnsupportedVector) {
                        const auto *rv_inst =
                            dynamic_cast<const RiscvISA::RiscvStaticInst *>(
                                static_inst.get());
                        panic_if(!rv_inst,
                                 "VPU offload requires a RISC-V instruction");
                        inst->fault =
                            std::make_shared<RiscvISA::IllegalInstFault>(
                                "Vector instruction is not supported by VPU",
                                rv_inst->machInst);
                    }
#endif
                }
                MinorDynInstPtr output_inst = inst;

                auto *dec_ptr =
                    cpu.getContext(inst->id.threadId)->getDecoderPtr();

                if (inst->isFault()) {
                    DPRINTF(Decode, "Fault being passed: %d\n",
                        inst->fault->name());

                    decode_info.inputIndex++;
                    decode_info.inMacroop = false;
                } else if (static_inst->isMacroop() &&
                           !(inst->decodedVectorOp.executionClass ==
                             VectorExecutionClass::VpuOffload)) {
                    // Las únicas instrucciones que no se descomponen son
                    // las que pasamos a la VPU
                    /* Generate a new micro-op */
                    StaticInstPtr static_micro_inst;

                    /* Set up PC for the next micro-op emitted */
                    if (!decode_info.inMacroop) {
                        set(decode_info.microopPC, *inst->pc);
                        decode_info.inMacroop = true;
                    }

                    if (isRomMicroPC(decode_info.microopPC->microPC())) {
                        static_micro_inst = dec_ptr->fetchRomMicroop(
                            decode_info.microopPC->microPC(), static_inst);
                    } else {
                        /* Get the micro-op static instruction from the
                         * static_inst. */
                        static_micro_inst = static_inst->fetchMicroop(
                            decode_info.microopPC->microPC());
                    }

                    output_inst =
                        new MinorDynInst(static_micro_inst, inst->id);
                    set(output_inst->pc, decode_info.microopPC);
                    output_inst->fault = NoFault;

                    /* Allow a predicted next address only on the last
                     *  microop */
                    if (static_micro_inst->isLastMicroop()) {
                        output_inst->predictedTaken = inst->predictedTaken;
                        set(output_inst->predictedTarget,
                                inst->predictedTarget);
                    }

                    DPRINTF(Decode, "Microop decomposition inputIndex:"
                        " %d output_index: %d lastMicroop: %s microopPC:"
                        " %s inst: %d\n",
                        decode_info.inputIndex, output_index,
                        (static_micro_inst->isLastMicroop() ?
                            "true" : "false"),
                        *decode_info.microopPC,
                        *output_inst);

                    /* Acknowledge that the static_inst isn't mine, it's my
                     * parent macro-op's */
                    parent_static_inst = static_inst;

                    static_micro_inst->advancePC(*decode_info.microopPC);

                    /* Step input if this is the last micro-op */
                    if (static_micro_inst->isLastMicroop()) {
                        decode_info.inputIndex++;
                        decode_info.inMacroop = false;
                    }
                } else {
                    /* Doesn't need decomposing, pass on instruction */
                    DPRINTF(Decode, "Passing on inst: %s inputIndex:"
                        " %d output_index: %d\n",
                        *output_inst, decode_info.inputIndex, output_index);

                    parent_static_inst = static_inst;

                    /* Step input */
                    decode_info.inputIndex++;
                    decode_info.inMacroop = false;
                }

                /* Set execSeqNum of output_inst */
                output_inst->id.execSeqNum = decode_info.execSeqNum;
                /* Add tracing */
#if TRACING_ON
                dynInstAddTracing(output_inst, parent_static_inst, cpu);
#endif

                /* Step to next sequence number */
                decode_info.execSeqNum++;

                /* Correctly size the output before writing */
                if (output_index == 0) insts_out.resize(outputWidth);
                /* Push into output */
                insts_out.insts[output_index] = output_inst;
                output_index++;
            }

            /* Have we finished with the input? */
            if (decode_info.inputIndex == insts_in->width()) {
                /* If we have just been producing micro-ops, we *must* have
                 * got to the end of that for inputIndex to be pushed past
                 * insts_in->width() */
                assert(!decode_info.inMacroop);
                popInput(tid);
                insts_in = NULL;

                if (processMoreThanOneInput) {
                    DPRINTF(Decode, "Wrapping\n");
                    insts_in = getInput(tid);
                }
            }
        }

        /* The rest of the output (if any) should already have been packed
         *  with bubble instructions by insts_out's initialisation
         *
         *  for (; output_index < outputWidth; output_index++)
         *      assert(insts_out.insts[output_index]->isBubble());
         */
    }

    /* If we generated output, reserve space for the result in the next stage
     *  and mark the stage as being active this cycle */
    if (!insts_out.isBubble()) {
        /* Note activity of following buffer */
        cpu.activityRecorder->activity();
        insts_out.threadId = tid;
        nextStageReserve[tid].reserve();
    }

    /* If we still have input to process and somewhere to put it,
     *  mark stage as active */
    for (ThreadID i = 0; i < cpu.numThreads; i++)
    {
        if (getInput(i) && nextStageReserve[i].canReserve()) {
            cpu.activityRecorder->activateStage(Pipeline::DecodeStageId);
            break;
        }
    }

    /* Make sure the input (if any left) is pushed */
    if (!inp.outputWire->isBubble())
        inputBuffer[inp.outputWire->threadId].pushTail();
}

inline ThreadID
Decode::getScheduledThread()
{
    /* Select thread via policy. */
    std::vector<ThreadID> priority_list;

    switch (cpu.threadPolicy) {
      case enums::SingleThreaded:
        priority_list.push_back(0);
        break;
      case enums::RoundRobin:
        priority_list = cpu.roundRobinPriority(threadPriority);
        break;
      case enums::Random:
        priority_list = cpu.randomPriority();
        break;
      default:
        panic("Unknown fetch policy");
    }

    for (auto tid : priority_list) {
        if (getInput(tid) && !decodeInfo[tid].blocked) {
            threadPriority = tid;
            return tid;
        }
    }

   return InvalidThreadID;
}

bool
Decode::isDrained()
{
    for (const auto &buffer : inputBuffer) {
        if (!buffer.empty())
            return false;
    }

    return (*inp.outputWire).isBubble();
}

void
Decode::minorTrace() const
{
    std::ostringstream data;

    if (decodeInfo[0].blocked)
        data << 'B';
    else
        (*out.inputWire).reportData(data);

    minor::minorTrace("insts=%s\n", data.str());
    inputBuffer[0].minorTrace();
}

} // namespace minor
} // namespace gem5
