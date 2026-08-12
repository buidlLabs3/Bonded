use authenticated_transfer_core::Instruction as TransferInstruction;
use bonded_inbox_program::{
    AUTHENTICATED_TRANSFER_PROGRAM_ID, BondState, Instruction, Outcome, STATE_VERSION,
};
use clock_core::{CLOCK_01_PROGRAM_ACCOUNT_ID, ClockAccountData};
use lee_core::{
    account::{AccountId, AccountWithMetadata},
    program::{
        AccountPostState, ChainedCall, Claim, PdaSeed, ProgramInput, ProgramOutput, read_lee_inputs,
    },
};
use risc0_zkvm::sha::{Impl, Sha256 as _};

const STATE_SEED_DOMAIN: [u8; 32] = *b"/Bonded/v1/State/00000000000000/";
const ESCROW_SEED_DOMAIN: [u8; 32] = *b"/Bonded/v1/Escrow/0000000000000/";

fn derive_seed(domain: &[u8; 32], bond_id: &[u8; 32]) -> PdaSeed {
    let mut bytes = [0_u8; 64];
    bytes[..32].copy_from_slice(domain);
    bytes[32..].copy_from_slice(bond_id);
    PdaSeed::new(
        Impl::hash_bytes(&bytes)
            .as_bytes()
            .try_into()
            .expect("SHA-256 output must be 32 bytes"),
    )
}

fn state_seed(bond_id: &[u8; 32]) -> PdaSeed {
    derive_seed(&STATE_SEED_DOMAIN, bond_id)
}

fn escrow_seed(bond_id: &[u8; 32]) -> PdaSeed {
    derive_seed(&ESCROW_SEED_DOMAIN, bond_id)
}

fn account_id(bytes: [u8; 32]) -> AccountId {
    AccountId::new(bytes)
}

fn initialize(
    self_program_id: [u32; 8],
    pre_states: Vec<AccountWithMetadata>,
    state: BondState,
) -> (
    Vec<AccountWithMetadata>,
    Vec<AccountPostState>,
    Vec<ChainedCall>,
) {
    let [sender, state_account, escrow_account, clock] = <[_; 4]>::try_from(pre_states)
        .expect("Initialize requires [sender, state PDA, escrow PDA, clock]");

    assert_eq!(
        clock.account_id, CLOCK_01_PROGRAM_ACCOUNT_ID,
        "Invalid clock account"
    );
    let now = ClockAccountData::from_bytes(&clock.account.data).timestamp;
    state
        .as_bond()
        .validate(now)
        .expect("invalid bond parameters");
    assert!(sender.is_authorized, "Sender must authorize initialization");
    assert_eq!(
        sender.account_id,
        account_id(state.sender),
        "Sender mismatch"
    );
    assert_eq!(
        sender.account.program_owner, AUTHENTICATED_TRANSFER_PROGRAM_ID,
        "Sender must be initialized for native transfers"
    );
    assert_eq!(
        state_account.account_id,
        AccountId::for_public_pda(&self_program_id, &state_seed(&state.id)),
        "Invalid state PDA"
    );
    assert_eq!(
        escrow_account.account_id,
        AccountId::for_public_pda(&self_program_id, &escrow_seed(&state.id)),
        "Invalid escrow PDA"
    );
    assert_eq!(
        state_account.account,
        Default::default(),
        "Bond already exists"
    );
    assert_eq!(
        escrow_account.account,
        Default::default(),
        "Escrow already exists"
    );
    assert!(
        sender.account.balance >= state.amount,
        "Sender has insufficient balance"
    );

    let mut state_post = state_account.account.clone();
    state_post.data = state
        .to_bytes()
        .try_into()
        .expect("bond state must fit in account data");
    let state_post = AccountPostState::new_claimed(state_post, Claim::Pda(state_seed(&state.id)));

    // Authenticated Transfer owns balance-bearing accounts. The Bonded program
    // authorizes this escrow PDA for the chained initialization transfer.
    let mut escrow_for_transfer = escrow_account.clone();
    escrow_for_transfer.is_authorized = true;
    let transfer = ChainedCall::new(
        AUTHENTICATED_TRANSFER_PROGRAM_ID,
        vec![sender.clone(), escrow_for_transfer],
        &TransferInstruction::Transfer {
            amount: state.amount,
        },
    )
    .with_pda_seeds(vec![escrow_seed(&state.id)]);

    (
        vec![
            sender.clone(),
            state_account,
            escrow_account.clone(),
            clock.clone(),
        ],
        vec![
            AccountPostState::new(sender.account),
            state_post,
            AccountPostState::new(escrow_account.account),
            AccountPostState::new(clock.account),
        ],
        vec![transfer],
    )
}

fn settle(
    self_program_id: [u32; 8],
    pre_states: Vec<AccountWithMetadata>,
    outcome: Outcome,
) -> (
    Vec<AccountWithMetadata>,
    Vec<AccountPostState>,
    Vec<ChainedCall>,
) {
    let [state_account, escrow_account, destination, authority, clock] =
        <[_; 5]>::try_from(pre_states)
            .expect("Settle requires [state PDA, escrow PDA, destination, authority, clock]");

    assert_eq!(
        clock.account_id, CLOCK_01_PROGRAM_ACCOUNT_ID,
        "Invalid clock account"
    );
    let now = ClockAccountData::from_bytes(&clock.account.data).timestamp;
    let mut state = BondState::from_bytes(&state_account.account.data);
    assert_eq!(
        state.version, STATE_VERSION,
        "Unsupported bond state version"
    );
    assert_eq!(
        state_account.account_id,
        AccountId::for_public_pda(&self_program_id, &state_seed(&state.id)),
        "Invalid state PDA"
    );
    assert_eq!(
        escrow_account.account_id,
        AccountId::for_public_pda(&self_program_id, &escrow_seed(&state.id)),
        "Invalid escrow PDA"
    );
    assert_eq!(state_account.account.program_owner, self_program_id);
    assert_eq!(
        escrow_account.account.program_owner, AUTHENTICATED_TRANSFER_PROGRAM_ID,
        "Escrow must be initialized for native transfers"
    );
    assert_eq!(
        destination.account.program_owner, AUTHENTICATED_TRANSFER_PROGRAM_ID,
        "Destination must be initialized for native transfers"
    );
    assert_eq!(
        escrow_account.account.balance, state.amount,
        "Escrow balance mismatch"
    );
    assert_eq!(
        authority.account_id,
        account_id(state.owner),
        "Owner authority mismatch"
    );

    let settlement = state
        .as_bond()
        .settle(outcome, now, authority.is_authorized)
        .expect("invalid settlement");
    assert_eq!(destination.account_id, account_id(settlement.destination));
    state.outcome = Some(settlement.outcome);

    let mut state_post = state_account.account.clone();
    state_post.data = state
        .to_bytes()
        .try_into()
        .expect("bond state must fit in account data");

    let mut escrow_for_transfer = escrow_account.clone();
    escrow_for_transfer.is_authorized = true;
    let transfer = ChainedCall::new(
        AUTHENTICATED_TRANSFER_PROGRAM_ID,
        vec![escrow_for_transfer, destination.clone()],
        &TransferInstruction::Transfer {
            amount: settlement.amount,
        },
    )
    .with_pda_seeds(vec![escrow_seed(&state.id)]);

    (
        vec![
            state_account,
            escrow_account.clone(),
            destination.clone(),
            authority.clone(),
            clock.clone(),
        ],
        vec![
            AccountPostState::new(state_post),
            AccountPostState::new(escrow_account.account),
            AccountPostState::new(destination.account),
            AccountPostState::new(authority.account),
            AccountPostState::new(clock.account),
        ],
        vec![transfer],
    )
}

fn main() {
    let (
        ProgramInput {
            self_program_id,
            caller_program_id,
            pre_states,
            instruction,
        },
        instruction_words,
    ) = read_lee_inputs::<Instruction>();
    assert!(caller_program_id.is_none(), "Top-level invocation required");

    let (pre_states, post_states, chained_calls) = match instruction {
        Instruction::Initialize {
            id,
            message_commitment,
            policy_commitment,
            sender,
            owner,
            sink,
            amount,
            deadline_ms,
        } => initialize(
            self_program_id,
            pre_states,
            BondState {
                version: STATE_VERSION,
                id,
                message_commitment,
                policy_commitment,
                sender,
                owner,
                sink,
                amount,
                deadline_ms,
                outcome: None,
            },
        ),
        Instruction::Settle { outcome } => settle(self_program_id, pre_states, outcome),
    };

    ProgramOutput::new(
        self_program_id,
        caller_program_id,
        instruction_words,
        pre_states,
        post_states,
    )
    .with_chained_calls(chained_calls)
    .write();
}
