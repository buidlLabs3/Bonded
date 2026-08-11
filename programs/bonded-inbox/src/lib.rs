#![cfg_attr(not(test), no_std)]

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
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
    AlreadySettled,
    NotExpired,
    UnauthorizedRejection,
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
    pub fn validate(&self, now: u64) -> Result<(), BondError> {
        if self.amount == 0 {
            return Err(BondError::ZeroAmount);
        }
        if self.deadline <= now {
            return Err(BondError::InvalidDeadline);
        }
        if self.owner == self.sink {
            return Err(BondError::OwnerIsSink);
        }
        if self.owner == self.sender {
            return Err(BondError::OwnerIsSender);
        }
        Ok(())
    }

    pub fn settle(
        &mut self,
        outcome: Outcome,
        now: u64,
        owner_authorized: bool,
        deterministic_violation: bool,
    ) -> Result<Settlement<N>, BondError> {
        if self.outcome.is_some() {
            return Err(BondError::AlreadySettled);
        }
        if outcome == Outcome::RefundExpired && now < self.deadline {
            return Err(BondError::NotExpired);
        }
        if outcome == Outcome::SinkRejected && !owner_authorized && !deterministic_violation {
            return Err(BondError::UnauthorizedRejection);
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
        let settlement = value
            .settle(Outcome::RefundAccepted, 100, false, false)
            .unwrap();
        assert_eq!(settlement.destination, *b"send");
        assert_eq!(settlement.amount, 25);
        assert_eq!(
            value.settle(Outcome::RefundAccepted, 100, false, false),
            Err(BondError::AlreadySettled)
        );
    }

    #[test]
    fn rejection_requires_authority_and_uses_sink() {
        let mut value = bond();
        assert_eq!(
            value.settle(Outcome::SinkRejected, 100, false, false),
            Err(BondError::UnauthorizedRejection)
        );
        let settlement = value
            .settle(Outcome::SinkRejected, 100, true, false)
            .unwrap();
        assert_eq!(settlement.destination, *b"sink");
    }

    #[test]
    fn expiry_cannot_settle_early() {
        let mut value = bond();
        assert_eq!(
            value.settle(Outcome::RefundExpired, 199, false, false),
            Err(BondError::NotExpired)
        );
        assert_eq!(
            value
                .settle(Outcome::RefundExpired, 200, false, false)
                .unwrap()
                .destination,
            *b"send"
        );
    }

    #[test]
    fn owner_can_never_be_sink_or_sender() {
        let mut value = bond();
        value.sink = value.owner;
        assert_eq!(value.validate(100), Err(BondError::OwnerIsSink));
        value = bond();
        value.sender = value.owner;
        assert_eq!(value.validate(100), Err(BondError::OwnerIsSender));
    }
}
