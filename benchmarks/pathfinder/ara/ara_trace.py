# Copyright (c) 2026 Félix Garcia Narocki (UCM)
# All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are
# met: redistributions of source code must retain the above copyright
# notice, this list of conditions and the following disclaimer;
# redistributions in binary form must reproduce the above copyright
# notice, this list of conditions and the following disclaimer in the
# documentation and/or other materials provided with the distribution;
# neither the name of the copyright holders nor the names of its
# contributors may be used to endorse or promote products derived from
# this software without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
# "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
# LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
# A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
# OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
# SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
# LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
# DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
# THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

"""Métricas de actividad de Ara a partir de una traza FST de Verilator.

Necesita ``pywellen`` y ``numpy`` (ver README.md). Lee las señales de
handshake del secuenciador, las lanes, el VRF y la VLSU de Ara y calcula,
dentro de la ventana vectorial (de la primera instrucción aceptada por el
secuenciador hasta que Ara queda inactiva tras la última):

- Instrucciones aceptadas, esperas de la CPU por el secuenciador e
  instrucciones en vuelo (media y máximo).
- Ciclos con alguna instrucción en vuelo en cada unidad (lanes, load,
  store, slide, máscaras).
- Por lane: palabras de 64 bits de resultado de ALU, slide y load,
  operandos de store y accesos de lectura/escritura a cada banco del VRF.
- AXI: ráfagas, beats y latencias AR -> primer/último R y AW -> B.
- Línea temporal de cada instrucción vectorial, en orden de programa:
  petición de la CPU, asignación en el secuenciador, aceptación y fin.

Convención de muestreo: el ciclo k es el intervalo [flanco k, flanco k+1)
del reloj; el valor de una señal en ese ciclo es el último volcado en o
antes del flanco k. Un handshake cuenta una vez por ciclo con valid y
ready a 1. El índice de ciclo es relativo al inicio de la traza.
"""

import argparse
import json
import sys

import numpy as np
import pywellen

ARA = "TOP.ara_tb_verilator.dut.i_ara_soc.i_system.i_ara"
NR_VINSN = 8
# Orden de vfu_offset_e en ara_pkg.sv tras las lanes.
PE_OFFSETS = {"load": 0, "store": 1, "mask": 2, "slide": 3}


class Trace:
    """Acceso a señales muestreadas por ciclo."""

    def __init__(self, path):
        self.wave = pywellen.Waveform(str(path))
        self.vars = {v.full_name: v for v in self.wave.all_vars()}
        times, values = self._changes(f"{ARA}.clk_i")
        self.edges = times[values == 1]

    def _changes(self, name):
        signal = self.vars[name].signal
        times = np.empty(len(signal), dtype=np.int64)
        values = []
        for index in range(len(signal)):
            time, value = signal[index]
            times[index] = time
            values.append(int(value))
        width = self.vars[name].bitwidth
        array = np.array(values, dtype=object if width > 62 else np.int64)
        return times, array

    def width(self, name):
        return self.vars[name].bitwidth

    def sample(self, name):
        """Valor de la señal en cada ciclo (array de objetos si es ancha)."""
        times, values = self._changes(name)
        index = np.searchsorted(times, self.edges, side="right") - 1
        result = values[np.clip(index, 0, None)]
        if values.dtype != object:
            result = np.where(index < 0, 0, result)
        return result


def popcount(array):
    return np.array([bin(int(x)).count("1") for x in array], dtype=np.int64)


def handshakes(trace, valid, ready):
    return np.nonzero(trace.sample(valid) & trace.sample(ready))[0]


def analyse(path, nr_lanes):
    trace = Trace(path)
    m = {"nr_lanes": nr_lanes, "trace_cycles": int(len(trace.edges))}
    seq = f"{ARA}.i_sequencer"

    req_valid = trace.sample(f"{seq}.ara_req_valid_i")
    req_ready = trace.sample(f"{seq}.ara_req_ready_o")
    idle = trace.sample(f"{ARA}.ara_idle")
    accepts = np.nonzero(req_valid & req_ready)[0]
    if len(accepts) == 0:
        raise ValueError(f"{path}: no vector instruction reached Ara")
    first, last = int(accepts[0]), int(accepts[-1])
    end = last + 1 + int(np.argmax(idle[last + 1 :] == 1))
    window = slice(first, end + 1)
    cycles = end + 1 - first
    m["window"] = {"first_cycle": first, "last_cycle": end, "cycles": cycles}

    def count(array):
        return int(np.sum(array[window]))

    # Secuenciador: aceptación, espera de la CPU e instrucciones en vuelo.
    running = trace.sample(f"{seq}.vinsn_running_q")
    in_flight = popcount(running[window])
    m["sequencer"] = {
        "insns_accepted": int(len(accepts)),
        "req_stall_cycles": count(req_valid & (1 - req_ready)),
        "ara_idle_cycles": count(idle),
        "outstanding_avg": float(in_flight.mean()),
        "outstanding_peak": int(in_flight.max()),
        "busy_cycles_any_insn": int(np.sum(in_flight > 0)),
    }

    # Línea temporal: cada identificador pasa por 0 entre dos usos (el
    # secuenciador sólo asigna identificadores libres en el ciclo actual),
    # así que cada subida es un inicio y cada bajada un fin.
    events = []
    for vid in range(NR_VINSN):
        bit = np.array([(int(x) >> vid) & 1 for x in running], dtype=np.int8)
        change = np.diff(np.concatenate(([0], bit, [0])))
        starts = np.nonzero(change == 1)[0]
        ends = np.nonzero(change == -1)[0]
        events += [(int(s), int(e), vid) for s, e in zip(starts, ends)]
    events.sort()
    if len(events) != len(accepts):
        raise ValueError(
            f"{path}: {len(events)} running intervals for {len(accepts)} "
            "accepted instructions"
        )
    # Petición: primer ciclo con valid tras el handshake anterior (la CPU
    # presenta la instrucción). Asignación (start) puede preceder a la
    # aceptación: el secuenciador reserva el identificador antes de
    # completar el handshake.
    valid_cycles = np.nonzero(req_valid)[0]
    previous = -1
    m["timeline"] = []
    for accept, (start, end_cycle, _vid) in zip(accepts, events):
        index = np.searchsorted(valid_cycles, previous, side="right")
        request = int(valid_cycles[index])
        m["timeline"].append(
            {
                "request": request,
                "start": start,
                "accept": int(accept),
                "end": end_cycle,
            }
        )
        previous = int(accept)

    # Ocupación por unidad (pe_vinsn_running_q: [NrPEs][NrVInsn]).
    pe_running = trace.sample(f"{seq}.pe_vinsn_running_q")
    units = {f"lane{lane}": lane for lane in range(nr_lanes)}
    units.update({name: nr_lanes + off for name, off in PE_OFFSETS.items()})
    m["units"] = {}
    for name, pe in units.items():
        mask = np.array(
            [(int(x) >> (pe * NR_VINSN)) & 0xFF for x in pe_running],
            dtype=np.int64,
        )
        occupancy = popcount(mask[window])
        m["units"][name] = {
            "busy_cycles": int(np.sum(occupancy > 0)),
            "outstanding_avg": float(occupancy.mean()),
            "outstanding_peak": int(occupancy.max()),
        }

    # Lanes: resultados y operandos de cada unidad y accesos al VRF.
    m["lanes"] = []
    for lane in range(nr_lanes):
        base = f"{ARA}.gen_lanes[{lane}].i_lane"
        operand_valid = trace.sample(f"{base}.i_vfus.alu_operand_valid_i")
        operand_ready = trace.sample(f"{base}.i_vfus.alu_operand_ready_o")
        requests = trace.sample(f"{base}.i_vrf.req_i")
        writes = trace.sample(f"{base}.i_vrf.wen_i")
        banks = trace.width(f"{base}.i_vrf.req_i")

        def hs(valid, ready):
            return count(
                trace.sample(f"{base}.{valid}")
                & trace.sample(f"{base}.{ready}")
            )

        m["lanes"].append(
            {
                "alu_operand_cycles": count(
                    ((operand_valid & operand_ready) != 0).astype(np.int64)
                ),
                "alu_result_words": hs("alu_result_req", "alu_result_gnt"),
                "slide_operand_words": hs(
                    "sldu_operand_valid_o", "sldu_operand_ready_i"
                ),
                "slide_result_words": hs(
                    "sldu_result_req_i", "sldu_result_gnt_o"
                ),
                "load_result_words": hs(
                    "ldu_result_req_i", "ldu_result_gnt_o"
                ),
                "store_operand_words": hs(
                    "stu_operand_valid_o", "stu_operand_ready_i"
                ),
                "vrf_reads": int(popcount((requests & ~writes)[window]).sum()),
                "vrf_writes": int(popcount((requests & writes)[window]).sum()),
                "vrf_bank_accesses": [
                    count((requests >> bank) & 1) for bank in range(banks)
                ],
                "vrf_busy_cycles": count((requests != 0).astype(np.int64)),
            }
        )

    # VLSU: AXI.
    vlsu = f"{ARA}.i_vlsu"
    ar = handshakes(
        trace,
        f"{vlsu}.i_addrgen.axi_ar_valid_o",
        f"{vlsu}.i_addrgen.axi_ar_ready_i",
    )
    aw = handshakes(
        trace,
        f"{vlsu}.i_addrgen.axi_aw_valid_o",
        f"{vlsu}.i_addrgen.axi_aw_ready_i",
    )
    r = handshakes(
        trace, f"{vlsu}.i_vldu.axi_r_valid_i", f"{vlsu}.i_vldu.axi_r_ready_o"
    )
    w = handshakes(
        trace, f"{vlsu}.i_vstu.axi_w_valid_o", f"{vlsu}.i_vstu.axi_w_ready_i"
    )
    b = handshakes(
        trace, f"{vlsu}.i_vstu.axi_b_valid_i", f"{vlsu}.i_vstu.axi_b_ready_o"
    )

    def summary(values):
        if not values:
            return None
        return {
            "median": float(np.median(values)),
            "min": int(min(values)),
            "p90": float(np.percentile(values, 90)),
            "max": int(max(values)),
        }

    # Las respuestas AXI de la VLSU llegan en orden (un único ID), así que
    # la ráfaga k recibe los beats R siguientes según su campo len. La
    # posición de len en ar_chan_t depende del ancho de user: se elige la
    # que hace cuadrar el total de beats.
    ar_chan = trace.sample(f"{vlsu}.i_addrgen.axi_ar_o")
    lengths = None
    for user_bits in (0, 1, 2):
        shift = user_bits + 21
        candidate = [((int(ar_chan[c]) >> shift) & 0xFF) + 1 for c in ar]
        if sum(candidate) == len(r):
            lengths = candidate
            break
    read_first, read_last = [], []
    if lengths is not None:
        beat = 0
        for request, length in zip(ar, lengths):
            read_first.append(int(r[beat] - request))
            read_last.append(int(r[beat + length - 1] - request))
            beat += length
    write = [int(resp - req) for req, resp in zip(aw, b)]

    def inside(events_array):
        return int(np.sum((events_array >= first) & (events_array <= end)))

    m["axi"] = {
        "beat_bytes": 4 * nr_lanes,
        "ar_bursts": inside(ar),
        "aw_bursts": inside(aw),
        "r_beats": inside(r),
        "w_beats": inside(w),
        "b_responses": inside(b),
        "burst_lengths_resolved": lengths is not None,
        "read_latency_ar_to_first_r": summary(read_first),
        "read_latency_ar_to_last_r": summary(read_last),
        "write_latency_aw_to_b": summary(write),
    }
    return m


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("fst")
    parser.add_argument("nr_lanes", type=int)
    parser.add_argument("output")
    args = parser.parse_args()
    metrics = analyse(args.fst, args.nr_lanes)
    with open(args.output, "w", encoding="utf-8") as stream:
        json.dump(metrics, stream, indent=1)
        stream.write("\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
