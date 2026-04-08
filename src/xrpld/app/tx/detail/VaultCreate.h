#ifndef XRPL_TX_VAULTCREATE_H_INCLUDED
#define XRPL_TX_VAULTCREATE_H_INCLUDED

#include <xrpld/app/tx/detail/Transactor.h>

#include <xrpl/basics/Slice.h>

#include <cstdint>
#include <optional>

namespace xrpl {

// Returns the number of owner-reserve increments a vault occupies.
// A plain vault occupies 1; each additional 500 bytes of VaultCode
// adds another increment (following the same rule as Smart Escrow).
std::int32_t
vaultReserveIncrements(std::optional<Slice> const& vaultCode);

class VaultCreate : public Transactor
{
public:
    static constexpr ConsequencesFactoryType ConsequencesFactory{Normal};

    explicit VaultCreate(ApplyContext& ctx) : Transactor(ctx)
    {
    }

    static bool
    checkExtraFeatures(PreflightContext const& ctx);

    static std::uint32_t
    getFlagsMask(PreflightContext const& ctx);

    static NotTEC
    preflight(PreflightContext const& ctx);

    static XRPAmount
    calculateBaseFee(ReadView const& view, STTx const& tx);

    static TER
    preclaim(PreclaimContext const& ctx);

    TER
    doApply() override;
};

}  // namespace xrpl

#endif
