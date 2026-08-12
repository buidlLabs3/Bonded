use bonded_inbox_program::{Instruction, Outcome};
use risc0_zkvm::sha::{Impl, Sha256 as _};
use risc0_zkvm::serde::to_vec;

fn initialize() -> Instruction {
    Instruction::Initialize {
        id: [1; 32],
        message_commitment: [2; 32],
        policy_commitment: [3; 32],
        sender: [4; 32],
        owner: [5; 32],
        sink: [6; 32],
        amount: 0x1122_3344_5566_7788_99aa_bbcc_ddee_ff00,
        deadline_ms: 0x0102_0304_0506_0708,
    }
}

#[test]
fn initialize_vector_matches_the_ffi_adapter_contract() {
    let words = to_vec(&initialize()).expect("instruction must serialize");
    assert_eq!(words.len(), 199);
    assert_eq!(&words[..4], &[0, 1, 1, 1]);
    assert_eq!(
        &words[193..],
        &[
            0xddee_ff00,
            0x99aa_bbcc,
            0x5566_7788,
            0x1122_3344,
            0x0506_0708,
            0x0102_0304,
        ]
    );
}

#[test]
fn settlement_vectors_match_the_ffi_adapter_contract() {
    for (outcome, tag) in [
        (Outcome::RefundAccepted, 0),
        (Outcome::SinkRejected, 1),
        (Outcome::RefundExpired, 2),
        (Outcome::RefundDeliveryFailed, 3),
    ] {
        let words = to_vec(&Instruction::Settle { outcome }).expect("instruction must serialize");
        assert_eq!(words, [1, tag]);
    }
}

#[test]
fn state_seed_digest_matches_standard_sha256_bytes() {
    let mut bytes = Vec::from(*b"/Bonded/v1/State/00000000000000/");
    bytes.extend([7_u8; 32]);
    assert_eq!(
        Impl::hash_bytes(&bytes).as_bytes(),
        &[
            0xda, 0x2a, 0x0a, 0x77, 0xe7, 0x19, 0x8b, 0xa0, 0x74, 0x0e, 0xcc, 0x16,
            0x5c, 0x2c, 0xa5, 0x9a, 0x4e, 0xe2, 0x6f, 0x1b, 0x7c, 0xe1, 0x11, 0x0b,
            0xab, 0xd0, 0x9d, 0xf2, 0xab, 0x6b, 0xaa, 0x13,
        ]
    );
}
