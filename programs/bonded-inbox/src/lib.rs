use borsh::{BorshDeserialize, BorshSerialize};
use serde::{Deserialize, Serialize};

pub const STATE_VERSION: u8 = 1;
pub const AUTHENTICATED_TRANSFER_PROGRAM_ID: [u32; 8] = [
    583_309_054,
    2_344_528_779,
    3_806_558_405,
    2_890_696_795,
    2_257_354_672,
    3_978_764_116,
    2_273_929_063,
    1_518_858_078,
];

#[derive(
    Clone, Copy, Debug, Eq, PartialEq, Serialize, Deserialize, BorshSerialize, BorshDeserialize,
)]
#[borsh(use_discriminant = true)]
#[repr(u8)]
pub enum Outcome {
    RefundAccepted = 1,
    SinkRejected = 2,
    RefundExpired = 3,
    RefundDeliveryFailed = 4,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum BondError {
    ZeroAmount,
    InvalidDeadline,
    OwnerIsSink,
    OwnerIsSender,
    SenderIsSink,
    AlreadySettled,
    NotExpired,
    UnauthorizedAcceptance,
    UnauthorizedRejection,
    UnauthorizedDeliveryFailure,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct Bond<const N: usize> {
    pub id: [u8; 32],
    pub message_commitment: [u8; 32],
    pub policy_commitment: [u8; 32],
    pub sender: [u8; N],
    pub owner: [u8; N],
    pub sink: [u8; N],
    pub amount: u128,
    pub deadline: u64,
    pub outcome: Option<Outcome>,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct Settlement<const N: usize> {
    pub destination: [u8; N],
    pub amount: u128,
    pub outcome: Outcome,
}

impl<const N: usize> Bond<N> {
    pub fn validate_static(&self) -> Result<(), BondError> {
        if self.amount == 0 {
            return Err(BondError::ZeroAmount);
        }
        if self.deadline == 0 {
            return Err(BondError::InvalidDeadline);
        }
        if self.owner == self.sink {
            return Err(BondError::OwnerIsSink);
        }
        if self.owner == self.sender {
            return Err(BondError::OwnerIsSender);
        }
        if self.sender == self.sink {
            return Err(BondError::SenderIsSink);
        }
        Ok(())
    }

    pub fn validate(&self, now: u64) -> Result<(), BondError> {
        self.validate_static()?;
        if self.deadline <= now {
            return Err(BondError::InvalidDeadline);
        }
        Ok(())
    }

    pub fn settle(
        &mut self,
        outcome: Outcome,
        now: u64,
        owner_authorized: bool,
    ) -> Result<Settlement<N>, BondError> {
        if outcome == Outcome::RefundExpired && now < self.deadline {
            return Err(BondError::NotExpired);
        }
        self.settle_with_external_time_enforcement(outcome, owner_authorized)
    }

    /// Settle after the caller binds expiry to a chain-enforced timestamp window.
    pub fn settle_with_external_time_enforcement(
        &mut self,
        outcome: Outcome,
        owner_authorized: bool,
    ) -> Result<Settlement<N>, BondError> {
        if self.outcome.is_some() {
            return Err(BondError::AlreadySettled);
        }
        match outcome {
            Outcome::RefundAccepted if !owner_authorized => {
                return Err(BondError::UnauthorizedAcceptance);
            }
            Outcome::SinkRejected if !owner_authorized => {
                return Err(BondError::UnauthorizedRejection);
            }
            Outcome::RefundDeliveryFailed if !owner_authorized => {
                return Err(BondError::UnauthorizedDeliveryFailure);
            }
            _ => {}
        }
        let destination = match outcome {
            Outcome::SinkRejected => self.sink,
            Outcome::RefundAccepted | Outcome::RefundExpired | Outcome::RefundDeliveryFailed => {
                self.sender
            }
        };
        self.outcome = Some(outcome);
        Ok(Settlement {
            destination,
            amount: self.amount,
            outcome,
        })
    }
}

#[derive(Clone, Debug, Eq, PartialEq, Serialize, Deserialize)]
pub enum Instruction {
    Initialize {
        id: [u8; 32],
        message_commitment: [u8; 32],
        policy_commitment: [u8; 32],
        sender: [u8; 32],
        owner: [u8; 32],
        sink: [u8; 32],
        amount: u128,
        deadline_ms: u64,
    },
    Settle {
        outcome: Outcome,
    },
}

#[derive(Clone, Debug, Eq, PartialEq, BorshSerialize, BorshDeserialize)]
pub struct BondState {
    pub version: u8,
    pub id: [u8; 32],
    pub message_commitment: [u8; 32],
    pub policy_commitment: [u8; 32],
    pub sender: [u8; 32],
    pub owner: [u8; 32],
    pub sink: [u8; 32],
    pub amount: u128,
    pub deadline_ms: u64,
    pub outcome: Option<Outcome>,
}

impl BondState {
    pub fn to_bytes(&self) -> Vec<u8> {
        borsh::to_vec(self).expect("bond state serialization should not fail")
    }

    pub fn from_bytes(bytes: &[u8]) -> Self {
        borsh::from_slice(bytes).expect("invalid bonded inbox state")
    }

    pub fn as_bond(&self) -> Bond<32> {
        Bond {
            id: self.id,
            message_commitment: self.message_commitment,
            policy_commitment: self.policy_commitment,
            sender: self.sender,
            owner: self.owner,
            sink: self.sink,
            amount: self.amount,
            deadline: self.deadline_ms,
            outcome: self.outcome,
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn bond() -> Bond<4> {
        Bond {
            id: [1; 32],
            message_commitment: [2; 32],
            policy_commitment: [3; 32],
            sender: *b"send",
            owner: *b"ownr",
            sink: *b"sink",
            amount: 25,
            deadline: 200,
            outcome: None,
        }
    }

    #[test]
    fn acceptance_refunds_sender_once() {
        let mut value = bond();
        assert_eq!(
            value.settle(Outcome::RefundAccepted, 100, false),
            Err(BondError::UnauthorizedAcceptance)
        );
        let settlement = value.settle(Outcome::RefundAccepted, 100, true).unwrap();
        assert_eq!(settlement.destination, *b"send");
        assert_eq!(settlement.amount, 25);
        assert_eq!(
            value.settle(Outcome::RefundAccepted, 100, true),
            Err(BondError::AlreadySettled)
        );
    }

    #[test]
    fn rejection_requires_authority_and_uses_sink() {
        let mut value = bond();
        assert_eq!(
            value.settle(Outcome::SinkRejected, 100, false),
            Err(BondError::UnauthorizedRejection)
        );
        let settlement = value.settle(Outcome::SinkRejected, 100, true).unwrap();
        assert_eq!(settlement.destination, *b"sink");
    }

    #[test]
    fn expiry_cannot_settle_early() {
        let mut value = bond();
        assert_eq!(
            value.settle(Outcome::RefundExpired, 199, false),
            Err(BondError::NotExpired)
        );
        assert_eq!(
            value
                .settle(Outcome::RefundExpired, 200, false)
                .unwrap()
                .destination,
            *b"send"
        );
    }

    #[test]
    fn externally_enforced_expiry_needs_no_owner_signature() {
        let mut value = bond();
        let settlement = value
            .settle_with_external_time_enforcement(Outcome::RefundExpired, false)
            .unwrap();
        assert_eq!(settlement.destination, *b"send");
        assert_eq!(settlement.amount, 25);
    }

    #[test]
    fn static_validation_rejects_an_unbounded_zero_deadline() {
        let mut value = bond();
        value.deadline = 0;
        assert_eq!(value.validate_static(), Err(BondError::InvalidDeadline));
    }

    #[test]
    fn owner_sender_and_sink_must_be_distinct() {
        let mut value = bond();
        value.sink = value.owner;
        assert_eq!(value.validate(100), Err(BondError::OwnerIsSink));
        value = bond();
        value.sender = value.owner;
        assert_eq!(value.validate(100), Err(BondError::OwnerIsSender));
        value = bond();
        value.sender = value.sink;
        assert_eq!(value.validate(100), Err(BondError::SenderIsSink));
    }

    #[test]
    fn state_encoding_is_roundtrippable_and_versioned() {
        let value = BondState {
            version: STATE_VERSION,
            id: [1; 32],
            message_commitment: [2; 32],
            policy_commitment: [3; 32],
            sender: [4; 32],
            owner: [5; 32],
            sink: [6; 32],
            amount: 25,
            deadline_ms: 200,
            outcome: None,
        };
        assert_eq!(BondState::from_bytes(&value.to_bytes()), value);
        assert_eq!(value.as_bond().validate(100), Ok(()));
    }
}
