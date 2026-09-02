"""Send R-APS PDU over interface"""

import argparse
import binascii
import enum

import scapy.arch as arch
import scapy.fields as fields
import scapy.layers.l2 as l2
import scapy.packet as packet
import scapy.sendrecv as sendrecv


class R_APS(packet.Packet):
    """R-APS"""

    name = "R-APS"
    fields_desc = [
        # CFM header
        fields.BitField("mel", 7, 3),
        fields.BitField("ver", 2, 5),
        fields.ByteField("opcode", 0x28),
        fields.ByteField("flags", 0),
        fields.ByteField("tlv_offset", 0x20),
        # R-APS info
        fields.BitField("request_state", 0x0E, 4),
        fields.BitField("subcode", 0, 4),
        fields.BitField("RB", 0, 1),
        fields.BitField("DNF", 0, 1),
        fields.BitField("BPR", 0, 1),
        fields.BitField("RFU_status", 0, 5),
        fields.MACField("node_id", binascii.unhexlify("000000000000")),
        fields.XBitField("RFU_pdu", 0, 192),
        fields.ByteField("tlv_end", 0),
    ]

    class Request(enum.IntEnum):
        """R-APS requests"""

        NR = 0x00
        MS = 0x07
        SF = 0x0B
        FS = 0x0D
        EVENT = 0x0E


def main() -> None:
    """Command line entrypoint"""

    packet.bind_layers(l2.Ether, R_APS, type=0x8902)

    parser = argparse.ArgumentParser()
    parser.add_argument(
        "interface", metavar="IFACE", type=str, help="Interface to send the frame over"
    )
    parser.add_argument(
        "request_state",
        metavar="REQUEST/STATE",
        type=str,
        choices=(v.name.lower() for v in R_APS.Request),
        help="The request to sent",
    )
    parser.add_argument(
        "--vid", metavar="VID", type=int, help="VLAN identifier", default=4093
    )
    parser.add_argument(
        "-r", "--ring-id", metavar="ID", type=int, help="Ring identifier", default=1
    )
    parser.add_argument("-b", "--rpl-blocked", action="store_true", help="Set RB flag")
    parser.add_argument(
        "-F", "--do-not-flush", action="store_true", help="Set DNF flag"
    )
    parser.add_argument("--bpr", type=int, default=0, help="Set blocked port reference")
    parser.add_argument(
        "-P", "--no-pad", action="store_true", help="Do not pad frame to 60 bytes"
    )
    parser.add_argument(
        "--mac_bcast",
        metavar="BCAST",
        default="0119",
        help="MAC multicast address block",
    )

    args = parser.parse_args()

    mac = arch.get_if_hwaddr(args.interface)

    assert 0 < args.ring_id < 240
    assert 0 <= args.vid <= 4093
    assert args.bpr in (0, 1)

    frame = l2.Ether(
        dst=binascii.unhexlify(f"{args.mac_bcast}a70000{args.ring_id:02x}"),
        src=binascii.unhexlify(mac.replace(":", "")),
    ) / R_APS(
        request_state=getattr(R_APS.Request, args.request_state.upper()),
        RB=args.rpl_blocked,
        DNF=args.do_not_flush,
        BPR=args.bpr,
        node_id=mac,
    )

    if not args.no_pad:
        frame = frame / packet.Padding(load="\x00" * (60 - len(bytes(frame))))

    sendrecv.srp(frame, iface=args.interface)


if __name__ == "__main__":
    main()
