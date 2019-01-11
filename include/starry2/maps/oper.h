template <class S>
inline void Map<S>::reset () 
{
    // Reset the cache
    cache.reset();

    // Reset Ylms
    y.setZero(N, ncoly);
    y_deg = 0;

    // Reset limb darkening
    u.setZero(lmax + 1, ncolu);
    setU0();
    u_deg = 0;

    // Reset the axis
    axis = yhat<Scalar>();
}

/**
Return the current highest spherical 
harmonic degree of the map.

*/
template <class S>
inline int Map<S>::getYDeg_ ()
{
    computeDegreeY();
    return y_deg;
}

/**
Return the current highest limb darkening
degree of the map.

*/
template <class S>
inline int Map<S>::getUDeg_ ()
{
    computeDegreeU();
    return u_deg;
}

/**
Check if the total degree of the map is valid.

*/
template <class S>
inline void Map<S>::checkDegree () 
{
    if (y_deg + u_deg > lmax) {
        cache.reset();
        y.setZero();
        y_deg = 0;
        u.setZero();
        setU0();
        u_deg = 0;
        throw errors::ValueError("Degree of the limb-darkened "
                                 "map exceeds `lmax`. All "
                                 "coefficients have been reset.");
    }
}

//! Compute the degree of the Ylm map.
template <class S>
inline void Map<S>::computeDegreeY () 
{
    if (cache.compute_degree_y) {
        y_deg = 0;
        for (int l = lmax; l >= 0; --l) {
            if ((y.block(l * l, 0, 2 * l + 1, ncoly).array() 
                    != 0.0).any()) {
                y_deg = l;
                break;
            }
        }
        checkDegree();
        cache.compute_degree_y = false;
    }
}

//! Compute the degree of the Ul map.
template <class S>
inline void Map<S>::computeDegreeU () 
{
    if (cache.compute_degree_u) {
        u_deg = 0;
        for (int l = lmax; l > 0; --l) {
            if (u.row(l).any()) {
                u_deg = l;
                break;
            }
        }
        checkDegree();
        cache.compute_degree_u = false;
    }
}

//! Compute the change of basis matrix from Ylms to pixels
template <class S>
inline void Map<S>::computeP (int res) {
    if (cache.compute_P || (cache.res != res)) {
        B.computePolyMatrix(res, cache.P);
        cache.res = res;
        cache.compute_P = false;
    }
}

//! Compute the zeta frame transform for Ylm rotations
template <class S>
inline void Map<S>::computeWigner () {
    if (cache.compute_Zeta) {
        W.updateZeta();
        W.updateYZeta();
        cache.compute_Zeta = false;
        cache.compute_YZeta = false;
    } else if (cache.compute_YZeta) {
        W.updateYZeta();
        cache.compute_YZeta = false;
    }
}

/**
Compute the Agol `c` basis and its derivative.
These are both normalized such that the total
unobscured flux is **unity**.

*/
template<class S>
inline void Map<S>::computeAgolGBasis () {
    if (cache.compute_agol_g) {
        limbdark::computeAgolGBasis(u, cache.agol_g, cache.dAgolGdu);
        normalizeAgolG(cache.agol_g, cache.dAgolGdu);
        cache.compute_agol_g = false;
    }
}

/**
Rotate the map *in place* by an angle `theta`.

*/
template <class S>
inline void Map<S>::rotate (
    const Scalar& theta
) 
{
    Scalar theta_rad = theta * radian;
    computeWigner();
    W.rotate(cos(theta_rad), sin(theta_rad));
    cache.mapRotated();
}

/**
Rotate the map by an angle `theta` and store
the result in `cache.Ry`. Optionally compute
and cache the Wigner rotation matrices and
their derivatives.

*/
template <class S>
inline void Map<S>::rotateIntoCache (
    const Scalar& theta,
    bool compute_matrices
) 
{
    Scalar theta_rad = theta * radian;
    computeWigner();
    if ((!compute_matrices) && (cache.theta != theta)) {
        W.rotate(cos(theta_rad), sin(theta_rad), cache.Ry);
        cache.theta = theta;
    } else if (compute_matrices && (cache.theta_with_grad != theta)) {
        W.compute(cos(theta_rad), sin(theta_rad));
        for (int l = 0; l < lmax + 1; ++l) {
            cache.Ry.block(l * l, 0, 2 * l + 1, ncoly) =
                W.R[l] * y.block(l * l, 0, 2 * l + 1, ncoly);
            cache.dRdthetay.block(l * l, 0, 2 * l + 1, ncoly) =
                W.dRdtheta[l] * y.block(l * l, 0, 2 * l + 1, ncoly);
        }
        cache.theta_with_grad = theta;
    }
}

/**
Rotate an arbitrary map vector in place
given an axis and an angle.

*/
template <class S>
inline void Map<S>::rotateByAxisAngle (
    const UnitVector<Scalar>& axis_,
    const Scalar& costheta,
    const Scalar& sintheta,
    YType& y_
) {
    Scalar tol = 10 * mach_eps<Scalar>();
    Scalar cosalpha, sinalpha, cosbeta, sinbeta, cosgamma, singamma;
    rotation::axisAngleToEuler(
        axis_(0), axis_(1), costheta, sintheta, tol,
        cosalpha, sinalpha, cosbeta, sinbeta, 
        cosgamma, singamma);
    rotation::rotar(
        lmax, cosalpha, sinalpha, 
        cosbeta, sinbeta, 
        cosgamma, singamma, tol, 
        cache.EulerD, cache.EulerR);
    for (int l = 0; l < lmax + 1; ++l) {
        y_.block(l * l, 0, 2 * l + 1, ncoly) =
            cache.EulerR[l] * y_.block(l * l, 0, 2 * l + 1, ncoly);
    }
}

/** 
Compute the limb darkening polynomial `agol_p`

NOTE: This is the **normalized** xyz polynomial corresponding
to the limb darkening vector `u`. The normalization is such 
that the total LUMINOSITY of the map remains unchanged. Note
that this could mean that the FLUX at any given viewing angle 
will be different from the non-limb-darkened version of the map.
This is significantly different from the beta version of the
code, which normalized it such that the FLUX remained unchanged.
That normalization was almost certainly unphysical.

*/
template <class S>
inline void Map<S>::computeLDPolynomial () {
    if (cache.compute_agol_p) {
        UType tmp = B.U1 * u;
        UCoeffType norm = (pi<Scalar>() * y.row(0)).cwiseQuotient(B.rT * tmp);
        cache.agol_p = tmp.array().rowwise() * norm.array();
        cache.compute_agol_p = false;
    }
}

/**
Limb-darken a polynomial map, and optionally compute the
gradient of the resulting map with respect to the input
polynomial map and the input limb-darkening map.

*/
template <class S>
inline void Map<S>::limbDarken (
    const YType& poly, 
    YType& poly_ld, 
    bool gradient
) {

    // Compute the limb darkening polynomial
    computeLDPolynomial();

    // Multiply a polynomial map by the LD polynomial
    computeDegreeY();
    computeDegreeU();
    if (gradient) {
        // TODO!
        throw errors::NotImplementedError("");
        // polymul(y_deg, poly, u_deg, p_u, lmax, poly_ld, dLDdp, dLDdp_u);
    } else {
        basis::polymul(y_deg, poly, u_deg, cache.agol_p, lmax, poly_ld);
    }

    // Compute the gradient of the limb-darkened polynomial
    // with respect to `y` and `u`. Tricky: make sure to get
    // the normalization correct.
    if (gradient) {
        // TODO!
        throw errors::NotImplementedError("");
    }

}

/**
Add a gaussian spot at a given latitude/longitude on the map.

*/
template <class S>
inline void Map<S>::addSpot (
    const YCoeffType& amp,
    const Scalar& sigma,
    const Scalar& lat,
    const Scalar& lon,
    int l
) {
    // Default degree is max degree
    if (l < 0) 
        l = lmax;
    if (l > lmax) 
        throw errors::ValueError("Invalid value for `l`.");

    // Compute the integrals recursively
    Vector<Scalar> IP(l + 1);
    Vector<Scalar> ID(l + 1);
    YType coeff(N, ncoly);
    coeff.setZero();

    // Constants
    Scalar a = 1.0 / (2 * sigma * sigma);
    Scalar sqrta = sqrt(a);
    Scalar erfa = erf(2 * sqrta);
    Scalar term = exp(-4 * a);

    // Seeding values
    IP(0) = root_pi<Scalar>() / (2 * sqrta) * erfa;
    IP(1) = (root_pi<Scalar>() * sqrta * erfa + term - 1) / (2 * a);
    ID(0) = 0;
    ID(1) = IP(0);

    // Recurse
    int sgn = -1;
    for (int n = 2; n < l + 1; ++n) {
        IP(n) = (2.0 * n - 1.0) / (2.0 * n * a) * (ID(n - 1) + sgn * term - 1.0) +
                (2.0 * n - 1.0) / n * IP(n - 1) - (n - 1.0) / n * IP(n - 2);
        ID(n) = (2.0 * n - 1.0) * IP(n - 1) + ID(n - 2);
        sgn *= -1;
    }

    // Compute the coefficients of the expansion
    // normalized so the integral over the sphere is `amp`
    for (int n = 0; n < l + 1; ++n)
        coeff.row(n * n + n) = 0.25 * amp * sqrt(2 * n + 1) * (IP(n) / IP(0));

    // Rotate the spot to the correct lat/lon
    // TODO: Speed this up with a single compound rotation
    Scalar lat_rad = lat * radian;
    Scalar lon_rad = lon * radian;
    rotateByAxisAngle(xhat<Scalar>(), cos(lat_rad), -sin(lat_rad), coeff);
    rotateByAxisAngle(yhat<Scalar>(), cos(lon_rad), sin(lon_rad), coeff);

    // Add this to the map
    cache.yChanged();
    y += coeff;
}

/**
Generate a random isotropic map with a given power spectrum.

*/
template <class S>
template <class V>
inline void Map<S>::random_ (
    const Vector<Scalar>& power,
    const V& seed,
    int col
) {
    int lmax_ = power.size() - 1;
    if (lmax_ > lmax) 
        lmax_ = lmax;
    int N_ = (lmax_ + 1) * (lmax_ + 1);

    // Generate N_ standard normal variables
    std::mt19937 gen(seed);
    std::normal_distribution<double> normal(0.0, 1.0);
    Vector<Scalar> vec(N_);
    for (int n = 0; n < N_; ++n)
        vec(n) = static_cast<Scalar>(normal(gen));

    // Zero degree
    vec(0) = sqrt(abs(power(0)));

    // Higher degrees
    for (int l = 1; l < lmax_ + 1; ++l) {
        vec.segment(l * l, 2 * l + 1) *=
            sqrt(abs(power(l)) / vec.segment(l * l, 2 * l + 1).squaredNorm());
    }

    // Set the vector
    if (col == -1) {
        y.block(0, 0, N_, ncoly) = vec.replicate(1, ncoly);
    } else {
        y.block(0, col, N_, 1) = vec;
    }

    cache.yChanged();
}